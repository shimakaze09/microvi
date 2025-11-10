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
#include <vector>

#include "commands/DeleteCommand.hpp"
#include "commands/QuitCommand.hpp"
#include "commands/WriteCommand.hpp"
#include "core/Buffer.hpp"
#include "core/KeyEvent.hpp"
#include "core/Mode.hpp"

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
}  // namespace

namespace core {
EditorApp::EditorApp() {
  ConfigureConsole();
  command_handler_.RegisterCommand(std::make_unique<commands::WriteCommand>());
  command_handler_.RegisterCommand(std::make_unique<commands::QuitCommand>());
  command_handler_.RegisterCommand(std::make_unique<commands::DeleteCommand>());
}

int EditorApp::Run(int argc, char** argv) {
  LoadFile(argc, argv);
  Render();

  while (state_.IsRunning()) {
    const KeyEvent kEvent = key_source_.Next();
    HandleEvent(kEvent);
  }

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
  const auto& buffer = state_.GetBuffer();
  const std::size_t kTotalLines = buffer.LineCount();
  const std::string kFileLabel =
      buffer.FilePath().empty() ? "[No Name]" : buffer.FilePath();
  const bool kIsDirty = buffer.IsDirty();
  const std::size_t kLineDigits =
      std::max<std::size_t>(1, std::to_string(kTotalLines).size());
  const std::size_t kPrefixWidth = 2 + kLineDigits + 1;

  std::ostringstream frame;
  frame << "\x1b[?25l" << "\x1b[H";

  frame << kFileLabel;
  if (kIsDirty) {
    frame << " [+]";
  }
  frame << "\x1b[K\n";

  for (std::size_t i = 0; i < kTotalLines; ++i) {
    const bool kIsCursor = i == state_.CursorLine();
    frame << (kIsCursor ? "> " : "  ");
    frame << std::setw(static_cast<int>(kLineDigits)) << (i + 1) << ' '
          << buffer.GetLine(i) << "\x1b[K\n";
  }

  std::ostringstream status_line;
  status_line << ModeLabel(state_.CurrentMode()) << ' ' << kFileLabel;
  if (kIsDirty) {
    status_line << " [+]";
  }
  status_line << "  Ln " << (state_.CursorLine() + 1) << ", Col "
              << (state_.CursorColumn() + 1);
  frame << status_line.str() << "\x1b[K\n";

  if (!state_.Status().empty()) {
    frame << state_.Status();
  }
  frame << "\x1b[K\n";

  if (state_.CurrentMode() == Mode::kCommandLine) {
    frame << kCommandPrefix << command_buffer_ << "\x1b[K";
  }

  frame << "\x1b[J";

  std::size_t cursor_row = 1;
  std::size_t cursor_column = 1;

  if (state_.CurrentMode() == Mode::kCommandLine) {
    const std::size_t kCommandRow = 1 + kTotalLines + 2 + 1;
    cursor_row = kCommandRow;
    cursor_column = 1 + 1 + command_buffer_.size();
  } else {
    cursor_row = 2 + state_.CursorLine();
    cursor_column = kPrefixWidth + state_.CursorColumn() + 1;
  }

  cursor_row = std::max<std::size_t>(1, cursor_row);
  cursor_column = std::max<std::size_t>(1, cursor_column);

  const std::string kFrameContent = frame.str();
  if (kFrameContent != previous_frame_) {
    std::cout << kFrameContent;
    previous_frame_ = kFrameContent;
  }

  std::cout << "\x1b[" << cursor_row << ';' << cursor_column << 'H'
            << "\x1b[?25h" << std::flush;
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
}  // namespace core