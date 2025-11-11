#include "core/EditorApp.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>


#include "commands/DeleteCommand.hpp"
#include "commands/QuitCommand.hpp"
#include "commands/WriteCommand.hpp"
#include "core/Buffer.hpp"
#include "core/Cursor.hpp"
#include "core/KeyEvent.hpp"
#include "core/Mode.hpp"
#include "core/Terminal.hpp"
#include "core/Theme.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr char kCommandPrefix = ':';

const char* ModeLabel(core::Mode mode) {
  switch (mode) {
    case core::Mode::kInsert:
      return "-- INSERT --";
    case core::Mode::kCommandLine:
      return "-- COMMAND --";
    case core::Mode::kNormal:
    default:
      return "-- NORMAL --";
  }
}

std::string TrimCopy(std::string_view value) {
  const auto kFirst = value.find_first_not_of(" \t\r\n");
  if (kFirst == std::string_view::npos) {
    return {};
  }
  const auto kLast = value.find_last_not_of(" \t\r\n");
  return std::string(value.substr(kFirst, kLast - kFirst + 1));
}

bool IsCommandSeparator(char ch) {
  return ch == '|' || ch == ';';
}

bool IsHighlightSeverity(core::StatusSeverity severity) {
  return severity == core::StatusSeverity::kWarning ||
         severity == core::StatusSeverity::kError;
}

const std::string& HighlightColor(const core::Theme& theme,
                                  core::StatusSeverity severity) {
  switch (severity) {
    case core::StatusSeverity::kWarning:
      return theme.status_warning;
    case core::StatusSeverity::kError:
      return theme.status_error;
    case core::StatusSeverity::kInfo:
    case core::StatusSeverity::kNone:
    default:
      return theme.status_info;
  }
}

std::string FitToWidth(std::string text, std::size_t width) {
  if (width == 0) {
    return {};
  }
  if (text.size() > width) {
    text.resize(width);
  }
  return text;
}

int ToSignedDelta(std::size_t count) {
  if (count == 0) {
    return 0;
  }
  constexpr std::size_t kMax =
      static_cast<std::size_t>((std::numeric_limits<int>::max)());
  if (count > kMax) {
    return (std::numeric_limits<int>::max)();
  }
  return static_cast<int>(count);
}
}  // namespace

namespace core {
EditorApp::EditorApp() {
  ConfigureConsole();
  command_handler_.RegisterCommand(std::make_unique<commands::WriteCommand>());
  command_handler_.RegisterCommand(std::make_unique<commands::QuitCommand>());
  command_handler_.RegisterCommand(std::make_unique<commands::DeleteCommand>());
}

int EditorApp::Run(int argc, char** argv) {
  PrepareScreen();
  LoadFile(argc, argv);
  StartInputLoop();
  Render();

  constexpr auto kFrameDuration = std::chrono::milliseconds(16);

  while (state_.IsRunning()) {
    const auto kFrameStart = std::chrono::steady_clock::now();
    ProcessPendingEvents();
    if (!state_.IsRunning()) {
      break;
    }

    Render();

    const auto kElapsed = std::chrono::steady_clock::now() - kFrameStart;
    if (kElapsed < kFrameDuration) {
      std::this_thread::sleep_for(kFrameDuration - kElapsed);
    }
  }

  StopInputLoop();
  RestoreScreen();
  return 0;
}

void EditorApp::LoadFile(int argc, char** argv) {
  auto& buffer = state_.GetBuffer();
  const std::span<char*> kArguments(argv, static_cast<std::size_t>(argc));

  if (kArguments.size() > 1) {
    const std::span<char*> kFileArgument = kArguments.subspan(1, 1);
    const char* path_cstr = kFileArgument.front();
    const std::string kPath =
        path_cstr != nullptr ? std::string(path_cstr) : std::string{};

    if (kPath.empty()) {
      state_.SetStatus("New Buffer", StatusSeverity::kInfo);
      return;
    }

    if (!buffer.LoadFromFile(kPath)) {
      std::cerr << "Failed to load file: " << kPath << '\n';
      buffer.SetFilePath(kPath);
      state_.SetStatus("New file", StatusSeverity::kInfo);
    } else {
      state_.SetStatus("Loaded file", StatusSeverity::kInfo);
    }
    return;
  }

  state_.SetStatus("New Buffer", StatusSeverity::kInfo);
}

void EditorApp::Render() const {
  const TerminalSize kSize = QueryTerminalSize();
  const std::size_t kTotalRows = std::max<std::size_t>(kSize.rows, 3);
  const std::size_t kTotalColumns = kSize.columns;

  const std::size_t kInfoRows = 2;
  const std::size_t kContentRows =
      kTotalRows > kInfoRows ? kTotalRows - kInfoRows : 0;

  UpdateScroll(kContentRows);

  const auto& buffer = state_.GetBuffer();
  const std::size_t kTotalLines = buffer.LineCount();
  const std::size_t kLineDigits = std::max<std::size_t>(
      1, std::to_string(std::max<std::size_t>(1, kTotalLines)).size());
  const std::size_t kPrefixWidth = 2 + kLineDigits + 1;

  std::ostringstream frame;
  frame << "\x1b[?25l" << "\x1b[H";

  for (std::size_t row = 0; row < kContentRows; ++row) {
    std::ostringstream line_stream;
    const std::size_t kLineIndex = scroll_offset_ + row;
    if (kLineIndex < kTotalLines) {
      const bool kIsCursorLine = kLineIndex == state_.CursorLine();
      line_stream << (kIsCursorLine ? "> " : "  ");
      line_stream << std::setw(static_cast<int>(kLineDigits))
                  << (kLineIndex + 1) << ' ' << buffer.GetLine(kLineIndex);
    } else {
      line_stream << "  " << std::string(kLineDigits, ' ') << ' ' << '~';
    }

    std::string line_text = FitToWidth(line_stream.str(), kTotalColumns);
    frame << line_text << "\x1b[K";
    frame << '\n';
  }

  if (kContentRows == 0) {
    frame << "\x1b[K\n";
  }

  const StatusSeverity kSeverity = state_.StatusLevel();
  const bool kHighlightStatus = IsHighlightSeverity(kSeverity);

  if (kHighlightStatus) {
    std::string highlight_text = FitToWidth(state_.Status(), kTotalColumns);
    const std::string& color = HighlightColor(theme_, kSeverity);
    const std::size_t kPadding = kTotalColumns > highlight_text.size()
                                     ? kTotalColumns - highlight_text.size()
                                     : 0;
    frame << color << highlight_text;
    if (kPadding > 0) {
      frame << std::string(kPadding, ' ');
    }
    frame << theme_.reset << "\x1b[K" << '\n';
  } else {
    std::ostringstream status_stream;
    const std::string kFileLabel =
        buffer.FilePath().empty() ? "[No Name]" : buffer.FilePath();
    status_stream << ModeLabel(state_.CurrentMode()) << ' ' << kFileLabel;
    if (buffer.IsDirty()) {
      status_stream << " [+]";
    }
    status_stream << "  Ln " << (state_.CursorLine() + 1) << ", Col "
                  << (state_.CursorColumn() + 1) << "  Lines " << kTotalLines;
    frame << FitToWidth(status_stream.str(), kTotalColumns) << "\x1b[K" << '\n';
  }

  std::string message_line;
  if (state_.CurrentMode() == Mode::kCommandLine) {
    message_line = std::string(1, kCommandPrefix) + command_buffer_;
  } else if (kSeverity == StatusSeverity::kInfo) {
    message_line = state_.Status();
  }
  frame << FitToWidth(message_line, kTotalColumns) << "\x1b[K";

  frame << "\x1b[J";

  Cursor cursor;
  if (state_.CurrentMode() == Mode::kCommandLine) {
    cursor.row = kContentRows + 2;
    cursor.column = 1 + 1 + command_buffer_.size();
  } else {
    if (kContentRows == 0) {
      cursor.row = 1;
    } else {
      const std::size_t kRelativeLine =
          state_.CursorLine() < scroll_offset_
              ? 0
              : state_.CursorLine() - scroll_offset_;
      cursor.row = std::min<std::size_t>(kRelativeLine + 1, kContentRows);
    }
    cursor.column = kPrefixWidth + state_.CursorColumn() + 1;
  }

  cursor.row = std::max<std::size_t>(1, cursor.row);
  cursor.column = std::max<std::size_t>(1, cursor.column);

  if (kTotalRows > 0 && cursor.row > kTotalRows) {
    cursor.row = kTotalRows;
  }
  if (kTotalColumns > 0 && cursor.column > kTotalColumns) {
    cursor.column = kTotalColumns;
  }

  const std::string kFrameContent = frame.str();
  if (kFrameContent != previous_frame_) {
    if (first_render_) {
      std::cout << "\x1b[2J";
    }
    std::cout << kFrameContent;
    previous_frame_ = kFrameContent;
  }

  std::cout << "\x1b[" << cursor.row << ';' << cursor.column << 'H'
            << "\x1b[?25h" << std::flush;
  first_render_ = false;
}

void EditorApp::HandleEvent(const KeyEvent& event) {
  switch (state_.CurrentMode()) {
    case Mode::kInsert:
      HandleInsertMode(event);
      break;
    case Mode::kCommandLine:
      HandleCommandMode(event);
      break;
    case Mode::kNormal:
    default:
      HandleNormalMode(event);
      break;
  }
}

void EditorApp::HandleNormalMode(const KeyEvent& event) {
  if (event.code == KeyCode::kEscape) {
    pending_normal_command_.clear();
    ResetCount();
    state_.ClearStatus();
    return;
  }

  if (event.code == KeyCode::kArrowDown) {
    if (pending_normal_command_ == "d") {
      pending_normal_command_.clear();
      const std::size_t kLines = std::max<std::size_t>(1, ConsumeCountOr(2));
      const std::size_t kDeleted =
          DeleteLineRange(state_.CursorLine(), kLines);
      if (kDeleted == 0) {
        state_.SetStatus("Delete failed", StatusSeverity::kWarning);
      } else {
        state_.MoveCursorLine(0);
        std::ostringstream message;
        message << "Deleted " << kDeleted << " line";
        if (kDeleted != 1) {
          message << 's';
        }
        state_.SetStatus(message.str(), StatusSeverity::kInfo);
      }
      return;
    }

    pending_normal_command_.clear();
    const std::size_t kCount = ConsumeCountOr(1);
    state_.MoveCursorLine(ToSignedDelta(kCount));
    state_.ClearStatus();
    return;
  }
  if (event.code == KeyCode::kArrowUp) {
    if (pending_normal_command_ == "d") {
      const std::size_t kLines = std::max<std::size_t>(1, ConsumeCountOr(2));
      const std::size_t kCurrent = state_.CursorLine();
      const std::size_t kStart = kLines > kCurrent + 1 ? 0 : kCurrent + 1 - kLines;
      pending_normal_command_.clear();
      const std::size_t kDeleted = DeleteLineRange(kStart, kLines);
      if (kDeleted == 0) {
        state_.SetStatus("Delete failed", StatusSeverity::kWarning);
      } else {
        state_.SetCursor(kStart, 0);
        state_.MoveCursorLine(0);
        std::ostringstream message;
        message << "Deleted " << kDeleted << " line";
        if (kDeleted != 1) {
          message << 's';
        }
        state_.SetStatus(message.str(), StatusSeverity::kInfo);
      }
      return;
    }

    pending_normal_command_.clear();
    const std::size_t kCount = ConsumeCountOr(1);
    state_.MoveCursorLine(-ToSignedDelta(kCount));
    state_.ClearStatus();
    return;
  }
  if (event.code == KeyCode::kArrowLeft) {
    pending_normal_command_.clear();
    const std::size_t kCount = ConsumeCountOr(1);
    state_.MoveCursorColumn(-ToSignedDelta(kCount));
    state_.ClearStatus();
    return;
  }
  if (event.code == KeyCode::kArrowRight) {
    pending_normal_command_.clear();
    const std::size_t kCount = ConsumeCountOr(1);
    state_.MoveCursorColumn(ToSignedDelta(kCount));
    state_.ClearStatus();
    return;
  }
  if (event.code != KeyCode::kCharacter) {
    pending_normal_command_.clear();
    ResetCount();
    state_.ClearStatus();
    return;
  }

  const char kValue = event.value;

  if (std::isdigit(static_cast<unsigned char>(kValue)) != 0) {
    if (!has_pending_count_ && pending_normal_command_.empty() &&
        kValue == '0') {
      state_.SetStatus("0 not mapped in normal mode", StatusSeverity::kWarning);
      ResetCount();
      return;
    }

    has_pending_count_ = true;
    pending_count_ =
        pending_count_ * 10 + static_cast<std::size_t>(kValue - '0');
    constexpr std::size_t kMaxCount = 1000000;
    if (pending_count_ > kMaxCount) {
      pending_count_ = kMaxCount;
    }

    state_.SetStatus(std::to_string(pending_count_), StatusSeverity::kInfo);
    return;
  }

  if (!pending_normal_command_.empty()) {
    const char kPending = pending_normal_command_.front();

    if (kPending == 'd') {
      if (kValue == 'd') {
        pending_normal_command_.clear();
        auto& buffer = state_.GetBuffer();
        const std::size_t kStartLine = state_.CursorLine();
        if (buffer.LineCount() == 0 || kStartLine >= buffer.LineCount()) {
          state_.SetStatus("Delete failed", StatusSeverity::kWarning);
          ResetCount();
          return;
        }

        const std::size_t kCount = ConsumeCountOr(1);
        std::size_t deleted = 0;
        for (std::size_t i = 0; i < kCount && kStartLine < buffer.LineCount();
             ++i) {
          if (buffer.DeleteLine(kStartLine)) {
            ++deleted;
          } else {
            break;
          }
        }

        if (deleted == 0) {
          state_.SetStatus("Delete failed", StatusSeverity::kWarning);
        } else {
          state_.MoveCursorLine(0);
          std::ostringstream message;
          message << "Deleted " << deleted << " line";
          if (deleted != 1) {
            message << 's';
          }
          state_.SetStatus(message.str(), StatusSeverity::kInfo);
        }
        return;
      }
      if (kValue == 'j') {
        pending_normal_command_.clear();
        const std::size_t kLines = std::max<std::size_t>(1, ConsumeCountOr(2));
        const std::size_t kDeleted =
            DeleteLineRange(state_.CursorLine(), kLines);
        if (kDeleted == 0) {
          state_.SetStatus("Delete failed", StatusSeverity::kWarning);
        } else {
          state_.MoveCursorLine(0);
          std::ostringstream message;
          message << "Deleted " << kDeleted << " line";
          if (kDeleted != 1) {
            message << 's';
          }
          state_.SetStatus(message.str(), StatusSeverity::kInfo);
        }
        return;
      }
      if (kValue == 'k') {
        const std::size_t kLines = std::max<std::size_t>(1, ConsumeCountOr(2));
        const std::size_t kCurrent = state_.CursorLine();
        const std::size_t kStart =
            kLines > kCurrent + 1 ? 0 : kCurrent + 1 - kLines;
        pending_normal_command_.clear();
        const std::size_t kDeleted = DeleteLineRange(kStart, kLines);
        if (kDeleted == 0) {
          state_.SetStatus("Delete failed", StatusSeverity::kWarning);
        } else {
          state_.SetCursor(kStart, 0);
          state_.MoveCursorLine(0);
          std::ostringstream message;
          message << "Deleted " << kDeleted << " line";
          if (kDeleted != 1) {
            message << 's';
          }
          state_.SetStatus(message.str(), StatusSeverity::kInfo);
        }
        return;
      }
      pending_normal_command_.clear();
      state_.SetStatus("d command requires motion", StatusSeverity::kWarning);
      ResetCount();
      return;
    }
    // Fall through for unmatched combinations so the current key is
    // processed normally.
  }

  switch (kValue) {
    case 'i':
      pending_normal_command_.clear();
      ResetCount();
      state_.SetMode(Mode::kInsert);
      state_.ClearStatus();
      break;
    case 'j':
      pending_normal_command_.clear();
      state_.MoveCursorLine(ToSignedDelta(ConsumeCountOr(1)));
      state_.ClearStatus();
      break;
    case 'k':
      pending_normal_command_.clear();
      state_.MoveCursorLine(-ToSignedDelta(ConsumeCountOr(1)));
      state_.ClearStatus();
      break;
    case 'h':
      pending_normal_command_.clear();
      state_.MoveCursorColumn(-ToSignedDelta(ConsumeCountOr(1)));
      state_.ClearStatus();
      break;
    case 'l':
      pending_normal_command_.clear();
      state_.MoveCursorColumn(ToSignedDelta(ConsumeCountOr(1)));
      state_.ClearStatus();
      break;
    case 'd':
      pending_normal_command_ = "d";
      if (has_pending_count_) {
        state_.SetStatus(std::to_string(pending_count_) + "d",
                         StatusSeverity::kInfo);
      } else {
        state_.SetStatus("d", StatusSeverity::kInfo);
      }
      break;
    case kCommandPrefix:
      pending_normal_command_.clear();
      ResetCount();
      state_.SetMode(Mode::kCommandLine);
      command_buffer_.clear();
      state_.ClearStatus();
      break;
    default:
      pending_normal_command_.clear();
      ResetCount();
      state_.SetStatus("Not mapped in normal mode", StatusSeverity::kWarning);
      break;
  }
}

void EditorApp::HandleInsertMode(const KeyEvent& event) {
  switch (event.code) {
    case KeyCode::kEscape:
      state_.SetMode(Mode::kNormal);
      state_.ClearStatus();
      return;
    case KeyCode::kEnter:
      InsertNewline();
      return;
    case KeyCode::kBackspace:
      HandleBackspace();
      return;
    case KeyCode::kArrowLeft:
      state_.MoveCursorColumn(-1);
      return;
    case KeyCode::kArrowRight:
      state_.MoveCursorColumn(1);
      return;
    case KeyCode::kArrowUp:
      state_.MoveCursorLine(-1);
      return;
    case KeyCode::kArrowDown:
      state_.MoveCursorLine(1);
      return;
    case KeyCode::kCharacter:
      if (std::isprint(static_cast<unsigned char>(event.value)) != 0) {
        InsertCharacter(event.value);
      }
      return;
    default:
      return;
  }
}

void EditorApp::HandleCommandMode(const KeyEvent& event) {
  switch (event.code) {
    case KeyCode::kEscape:
      command_buffer_.clear();
      state_.SetMode(Mode::kNormal);
      state_.ClearStatus();
      return;
    case KeyCode::kEnter: {
      if (command_buffer_.empty()) {
        state_.SetStatus("Command line empty", StatusSeverity::kWarning);
      } else if (!ExecuteCommandLine(command_buffer_)) {
        state_.SetStatus("Unknown command", StatusSeverity::kWarning);
      }
      command_buffer_.clear();
      state_.SetMode(Mode::kNormal);
      return;
    }
    case KeyCode::kBackspace:
      if (!command_buffer_.empty()) {
        command_buffer_.pop_back();
      }
      return;
    case KeyCode::kCharacter:
      if (std::isprint(static_cast<unsigned char>(event.value)) != 0) {
        command_buffer_.push_back(event.value);
      }
      return;
    default:
      return;
  }
}

void EditorApp::HandleBackspace() {
  auto& buffer = state_.GetBuffer();
  const std::size_t kLine = state_.CursorLine();
  const std::size_t kColumn = state_.CursorColumn();

  if (kColumn > 0) {
    if (buffer.DeleteChar(kLine, kColumn)) {
      state_.SetCursor(kLine, kColumn - 1);
    }
    return;
  }

  if (kLine == 0) {
    return;
  }

  std::string current_line = buffer.GetLine(kLine);
  if (!buffer.DeleteLine(kLine)) {
    return;
  }

  auto& previous = buffer.GetLine(kLine - 1);
  const std::size_t kPreviousLength = previous.size();
  previous += current_line;
  state_.SetCursor(kLine - 1, kPreviousLength);
}

void EditorApp::InsertCharacter(char value) {
  auto& buffer = state_.GetBuffer();
  const std::size_t kLine = state_.CursorLine();
  const std::size_t kColumn = state_.CursorColumn();

  if (buffer.InsertChar(kLine, kColumn, value)) {
    state_.SetCursor(kLine, kColumn + 1);
  }
}

void EditorApp::InsertNewline() {
  auto& buffer = state_.GetBuffer();
  const std::size_t kLine = state_.CursorLine();
  const std::size_t kColumn = state_.CursorColumn();

  auto& current = buffer.GetLine(kLine);
  std::string tail = current.substr(kColumn);
  current.erase(kColumn);
  if (!buffer.InsertLine(kLine + 1, tail)) {
    state_.SetStatus("Insert failed", StatusSeverity::kError);
    return;
  }

  state_.SetCursor(kLine + 1, 0);
}

void EditorApp::ConfigureConsole() {
#ifdef _WIN32
  HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle == INVALID_HANDLE_VALUE) {
    return;
  }

  DWORD mode = 0;
  if (GetConsoleMode(handle, &mode) == 0) {
    return;
  }

  if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
    SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }
#endif
}

void EditorApp::PrepareScreen() {
  if (screen_prepared_) {
    return;
  }

  previous_frame_.clear();
  first_render_ = true;
  screen_prepared_ = true;
}

void EditorApp::RestoreScreen() {
  if (!screen_prepared_) {
    return;
  }

  std::cout << "\x1b[?25h\x1b[0m\x1b[2J\x1b[H" << std::flush;
  screen_prepared_ = false;
  previous_frame_.clear();
  first_render_ = true;
}

void EditorApp::StartInputLoop() {
  StopInputLoop();
  input_thread_ =
      std::jthread([this](const std::stop_token& token) { InputLoop(token); });
}

void EditorApp::StopInputLoop() {
  if (input_thread_.joinable()) {
    input_thread_.request_stop();
    input_thread_.join();
  }
}

void EditorApp::InputLoop(const std::stop_token& token) {
  while (!token.stop_requested()) {
    KeyEvent event{};
    if (key_source_.Poll(event)) {
      event_queue_.Push(event);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
}

void EditorApp::ProcessPendingEvents() {
  auto events = event_queue_.ConsumeAll();
  for (const KeyEvent& event : events) {
    HandleEvent(event);
    if (!state_.IsRunning()) {
      break;
    }
  }
}

void EditorApp::ResetCount() noexcept {
  pending_count_ = 0;
  has_pending_count_ = false;
}

std::size_t EditorApp::ConsumeCountOr(std::size_t fallback) noexcept {
  if (!has_pending_count_ || pending_count_ == 0) {
    ResetCount();
    return fallback;
  }

  std::size_t count = pending_count_;
  ResetCount();
  return count;
}

std::size_t EditorApp::DeleteLineRange(std::size_t start_line,
                                       std::size_t line_count) {
  if (line_count == 0) {
    return 0;
  }

  auto& buffer = state_.GetBuffer();
  if (buffer.LineCount() == 0 || start_line >= buffer.LineCount()) {
    return 0;
  }

  std::size_t deleted = 0;
  for (std::size_t i = 0; i < line_count && start_line < buffer.LineCount();
       ++i) {
    if (buffer.DeleteLine(start_line)) {
      ++deleted;
    } else {
      break;
    }
  }

  return deleted;
}

bool EditorApp::ExecuteCommandLine(const std::string& line) {
  const std::string kTrimmed = TrimCopy(line);
  if (kTrimmed.empty()) {
    return false;
  }

  std::vector<std::string> commands;
  auto push_command = [&commands](const std::string& candidate) {
    std::string command = TrimCopy(candidate);
    if (command.empty()) {
      return;
    }

    if (command == "wq" || command == "qw" || command == "x") {
      commands.emplace_back(":w");
      commands.emplace_back(":q");
      return;
    }

    if (command.front() != ':') {
      command.insert(command.begin(), ':');
    }
    commands.push_back(std::move(command));
  };

  std::string segment;
  bool saw_separator = false;
  for (char ch : line) {
    if (IsCommandSeparator(ch)) {
      saw_separator = true;
      push_command(segment);
      segment.clear();
    } else {
      segment.push_back(ch);
    }
  }
  push_command(segment);

  if (!saw_separator && commands.empty()) {
    push_command(kTrimmed);
  }

  if (commands.empty()) {
    return false;
  }

  bool success = true;
  for (const std::string& command : commands) {
    if (!command_handler_.Handle(state_, command)) {
      success = false;
      break;
    }
    if (!state_.IsRunning()) {
      break;
    }
  }

  return success;
}

void EditorApp::UpdateScroll(std::size_t content_rows) const {
  if (content_rows == 0) {
    scroll_offset_ = 0;
    return;
  }

  const auto& buffer = state_.GetBuffer();
  const std::size_t kTotalLines = buffer.LineCount();

  if (kTotalLines == 0) {
    scroll_offset_ = 0;
    return;
  }

  if (scroll_offset_ >= kTotalLines) {
    scroll_offset_ = kTotalLines - 1;
  }

  const std::size_t kCursorLine = state_.CursorLine();
  if (kCursorLine < scroll_offset_) {
    scroll_offset_ = kCursorLine;
  } else if (kCursorLine >= scroll_offset_ + content_rows) {
    scroll_offset_ = kCursorLine - content_rows + 1;
  }

  const std::size_t kMaxOffset =
      kTotalLines > content_rows ? kTotalLines - content_rows : 0;
  if (scroll_offset_ > kMaxOffset) {
    scroll_offset_ = kMaxOffset;
  }
}
}  // namespace core