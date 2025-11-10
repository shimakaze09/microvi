#include "core/EditorApp.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
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

std::string FitToWidth(std::string text, std::size_t width) {
  if (width == 0) {
    return {};
  }
  if (text.size() > width) {
    text.resize(width);
  }
  return text;
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
  Render();

  while (state_.IsRunning()) {
    const KeyEvent kEvent = key_source_.Next();
    HandleEvent(kEvent);
  }

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
      state_.SetStatus("New Buffer");
      return;
    }

    if (!buffer.LoadFromFile(kPath)) {
      std::cerr << "Failed to load file: " << kPath << '\n';
      buffer.SetFilePath(kPath);
      state_.SetStatus("New file");
    } else {
      state_.SetStatus("Loaded file");
    }
    return;
  }

  state_.SetStatus("New Buffer");
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

  std::string info_line;
  if (state_.CurrentMode() == Mode::kCommandLine) {
    info_line = std::string(1, kCommandPrefix) + command_buffer_;
  } else {
    info_line = state_.Status();
  }
  frame << FitToWidth(std::move(info_line), kTotalColumns) << "\x1b[K";

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

  Render();
}

void EditorApp::HandleNormalMode(const KeyEvent& event) {
  if (event.code == KeyCode::kEscape) {
    if (!pending_normal_command_.empty()) {
      pending_normal_command_.clear();
      state_.SetStatus("Command cancelled");
    }
    return;
  }

  if (event.code == KeyCode::kArrowDown) {
    pending_normal_command_.clear();
    state_.MoveCursorLine(1);
    state_.SetStatus("Moved down");
    return;
  }
  if (event.code == KeyCode::kArrowUp) {
    pending_normal_command_.clear();
    state_.MoveCursorLine(-1);
    state_.SetStatus("Moved up");
    return;
  }
  if (event.code == KeyCode::kArrowLeft) {
    pending_normal_command_.clear();
    state_.MoveCursorColumn(-1);
    state_.SetStatus("Moved left");
    return;
  }
  if (event.code == KeyCode::kArrowRight) {
    pending_normal_command_.clear();
    state_.MoveCursorColumn(1);
    state_.SetStatus("Moved right");
    return;
  }
  if (event.code != KeyCode::kCharacter) {
    pending_normal_command_.clear();
    return;
  }

  const char kValue = event.value;

  if (!pending_normal_command_.empty()) {
    const char kPending = pending_normal_command_.front();
    pending_normal_command_.clear();

    if (kPending == 'd') {
      if (kValue == 'd') {
        auto& buffer = state_.GetBuffer();
        const std::size_t kTargetLine = state_.CursorLine();
        if (buffer.DeleteLine(kTargetLine)) {
          state_.MoveCursorLine(0);
          state_.SetStatus("Deleted line");
        } else {
          state_.SetStatus("Delete failed");
        }
        return;
      }
    }
    // Fall through for unmatched combinations so the current key is
    // processed normally.
  }

  switch (kValue) {
    case 'i':
      pending_normal_command_.clear();
      state_.SetMode(Mode::kInsert);
      state_.SetStatus("Insert mode (ESC to leave)");
      break;
    case 'j':
      pending_normal_command_.clear();
      state_.MoveCursorLine(1);
      state_.SetStatus("Moved down");
      break;
    case 'k':
      pending_normal_command_.clear();
      state_.MoveCursorLine(-1);
      state_.SetStatus("Moved up");
      break;
    case 'h':
      pending_normal_command_.clear();
      state_.MoveCursorColumn(-1);
      state_.SetStatus("Moved left");
      break;
    case 'l':
      pending_normal_command_.clear();
      state_.MoveCursorColumn(1);
      state_.SetStatus("Moved right");
      break;
    case 'd':
      pending_normal_command_ = "d";
      state_.SetStatus("d");
      break;
    case kCommandPrefix:
      pending_normal_command_.clear();
      state_.SetMode(Mode::kCommandLine);
      command_buffer_.clear();
      state_.SetStatus("Command mode");
      break;
    default:
      pending_normal_command_.clear();
      state_.SetStatus("Input ignored in normal mode");
      break;
  }
}

void EditorApp::HandleInsertMode(const KeyEvent& event) {
  switch (event.code) {
    case KeyCode::kEscape:
      state_.SetMode(Mode::kNormal);
      state_.SetStatus("Exited insert mode");
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
      state_.SetStatus("Command cancelled");
      return;
    case KeyCode::kEnter: {
      if (command_buffer_.empty()) {
        state_.SetStatus("Command line empty");
      } else if (!ExecuteCommandLine(command_buffer_)) {
        state_.SetStatus("Unknown command");
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
    state_.SetStatus("Insert failed");
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