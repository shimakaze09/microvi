#include "core/EditorApp.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
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

struct TextPosition {
  std::size_t line = 0;
  std::size_t column = 0;
};

struct FindParams {
  char target = 0;
  std::size_t count = 1;
  std::size_t start_column = 0;
};

enum class FindOperationKind : std::uint8_t {
  kForwardTo,
  kForwardTill,
  kBackwardTo,
  kBackwardTill,
};

struct FindMotionResult {
  TextPosition cursor{};
  std::size_t matched_column = 0;
  bool include_target_char = false;
  bool backward = false;
};

FindOperationKind FindKindFromCommand(char command) {
  switch (command) {
    case 'f':
      return FindOperationKind::kForwardTo;
    case 'F':
      return FindOperationKind::kBackwardTo;
    case 't':
      return FindOperationKind::kForwardTill;
    case 'T':
      return FindOperationKind::kBackwardTill;
    default:
      return FindOperationKind::kForwardTo;
  }
}

char CommandFromState(bool backward, bool till) {
  if (!backward && !till) {
    return 'f';
  }
  if (!backward && till) {
    return 't';
  }
  if (backward && !till) {
    return 'F';
  }
  return 'T';
}

bool operator==(const TextPosition& lhs, const TextPosition& rhs) {
  return lhs.line == rhs.line && lhs.column == rhs.column;
}

bool operator!=(const TextPosition& lhs, const TextPosition& rhs) {
  return !(lhs == rhs);
}

bool IsWordChar(char ch) {
  const unsigned char kCharacter = static_cast<unsigned char>(ch);
  return std::isalnum(kCharacter) != 0 || kCharacter == '_';
}

bool IsBlankLine(std::string_view line) {
  for (unsigned char ch : line) {
    if (std::isspace(ch) == 0) {
      return false;
    }
  }
  return true;
}

TextPosition ClampPosition(const core::Buffer& buffer, TextPosition position) {
  if (buffer.LineCount() == 0) {
    return position;
  }

  if (position.line >= buffer.LineCount()) {
    position.line = buffer.LineCount() - 1;
  }

  const std::string& line = buffer.GetLine(position.line);
  if (position.column > line.size()) {
    position.column = line.size();
  }

  return position;
}

TextPosition NextWordStart(const core::Buffer& buffer, TextPosition position) {
  if (buffer.LineCount() == 0) {
    return position;
  }

  position = ClampPosition(buffer, position);

  bool consumed_segment = false;

  while (position.line < buffer.LineCount()) {
    const std::string& line = buffer.GetLine(position.line);
    const std::size_t kLineLength = line.size();

    if (position.column >= kLineLength) {
      if (position.line + 1 >= buffer.LineCount()) {
        return TextPosition{position.line, kLineLength};
      }
      position.line += 1;
      position.column = 0;
      consumed_segment = false;
      continue;
    }

    const unsigned char kCurrentChar =
        static_cast<unsigned char>(line.at(position.column));
    if (std::isspace(kCurrentChar) != 0) {
      consumed_segment = false;
      position.column += 1;
      continue;
    }

    if (!consumed_segment) {
      const bool kInitialIsWord = IsWordChar(static_cast<char>(kCurrentChar));
      consumed_segment = true;
      while (position.column < kLineLength) {
        const unsigned char kNextChar =
            static_cast<unsigned char>(line.at(position.column));
        if (std::isspace(kNextChar) != 0) {
          break;
        }
        const bool kNextIsWord = IsWordChar(static_cast<char>(kNextChar));
        if (kNextIsWord != kInitialIsWord) {
          break;
        }
        position.column += 1;
      }
      continue;
    }

    return position;
  }

  const std::size_t kLastLineIndex = buffer.LineCount() - 1;
  return TextPosition{kLastLineIndex, buffer.GetLine(kLastLineIndex).size()};
}

TextPosition NextBigWordStart(const core::Buffer& buffer,
                              TextPosition position) {
  if (buffer.LineCount() == 0) {
    return position;
  }

  position = ClampPosition(buffer, position);
  bool consumed_segment = false;

  while (position.line < buffer.LineCount()) {
    const std::string& line = buffer.GetLine(position.line);
    const std::size_t kLineLength = line.size();

    if (position.column >= kLineLength) {
      if (position.line + 1 >= buffer.LineCount()) {
        return TextPosition{position.line, kLineLength};
      }
      position.line += 1;
      position.column = 0;
      consumed_segment = false;
      continue;
    }

    const unsigned char kCurrentChar =
        static_cast<unsigned char>(line.at(position.column));
    if (std::isspace(kCurrentChar) != 0) {
      consumed_segment = false;
      position.column += 1;
      continue;
    }

    if (!consumed_segment) {
      consumed_segment = true;
      while (position.column < kLineLength) {
        const unsigned char kNextChar =
            static_cast<unsigned char>(line.at(position.column));
        if (std::isspace(kNextChar) != 0) {
          break;
        }
        position.column += 1;
      }
      continue;
    }

    return position;
  }

  const std::size_t kLastLineIndex = buffer.LineCount() - 1;
  return TextPosition{kLastLineIndex, buffer.GetLine(kLastLineIndex).size()};
}

TextPosition PreviousBigWordStart(const core::Buffer& buffer,
                                  TextPosition position) {
  if (buffer.LineCount() == 0) {
    return position;
  }

  position = ClampPosition(buffer, position);

  auto retreat_line = [&]() {
    if (position.line == 0) {
      position.column = 0;
      return false;
    }
    position.line -= 1;
    position.column = buffer.GetLine(position.line).size();
    return true;
  };

  if (position.column > 0) {
    position.column -= 1;
  } else if (!retreat_line()) {
    return TextPosition{0, 0};
  }

  while (true) {
    const std::string& line = buffer.GetLine(position.line);
    const std::size_t kLineLength = line.size();
    if (kLineLength == 0) {
      if (!retreat_line()) {
        return TextPosition{0, 0};
      }
      continue;
    }

    if (position.column >= kLineLength) {
      position.column = kLineLength - 1;
    }

    const unsigned char kCurrentChar =
        static_cast<unsigned char>(line.at(position.column));
    if (std::isspace(kCurrentChar) != 0) {
      if (position.column == 0) {
        if (!retreat_line()) {
          return TextPosition{0, 0};
        }
      } else {
        position.column -= 1;
      }
      continue;
    }

    while (position.column > 0) {
      const unsigned char kPrevChar =
          static_cast<unsigned char>(line.at(position.column - 1));
      if (std::isspace(kPrevChar) != 0) {
        break;
      }
      position.column -= 1;
    }

    return position;
  }
}

TextPosition BigWordEndInclusive(const core::Buffer& buffer,
                                 TextPosition position) {
  if (buffer.LineCount() == 0) {
    return position;
  }

  position = ClampPosition(buffer, position);

  while (position.line < buffer.LineCount()) {
    const std::string& line = buffer.GetLine(position.line);
    const std::size_t kLineLength = line.size();

    if (position.column >= kLineLength) {
      if (position.line + 1 >= buffer.LineCount()) {
        return TextPosition{position.line, kLineLength};
      }
      position.line += 1;
      position.column = 0;
      continue;
    }

    const unsigned char kCurrentChar =
        static_cast<unsigned char>(line.at(position.column));
    if (std::isspace(kCurrentChar) != 0) {
      position.column += 1;
      continue;
    }

    std::size_t probe = position.column;
    while (probe < kLineLength) {
      const unsigned char kProbeChar =
          static_cast<unsigned char>(line.at(probe));
      if (std::isspace(kProbeChar) != 0) {
        break;
      }
      probe += 1;
    }

    if (probe == position.column) {
      return position;
    }

    return TextPosition{position.line, probe - 1};
  }

  const std::size_t kLastLineIndex = buffer.LineCount() - 1;
  return TextPosition{kLastLineIndex, buffer.GetLine(kLastLineIndex).size()};
}

std::size_t FirstNonBlankColumn(const std::string& line) {
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (std::isspace(static_cast<unsigned char>(line[i])) == 0) {
      return i;
    }
  }
  return 0;
}

std::size_t LastNonBlankColumn(const std::string& line) {
  if (line.empty()) {
    return 0;
  }

  for (std::size_t i = line.size(); i-- > 0;) {
    if (std::isspace(static_cast<unsigned char>(line[i])) == 0) {
      return i;
    }
  }

  return 0;
}

TextPosition FirstNonBlankPosition(const core::Buffer& buffer,
                                   std::size_t line) {
  if (buffer.LineCount() == 0) {
    return TextPosition{};
  }
  line = (std::min)(line, buffer.LineCount() - 1);
  const std::string& text = buffer.GetLine(line);
  return TextPosition{line, FirstNonBlankColumn(text)};
}

TextPosition LastNonBlankPosition(const core::Buffer& buffer,
                                  std::size_t line) {
  if (buffer.LineCount() == 0) {
    return TextPosition{};
  }
  line = (std::min)(line, buffer.LineCount() - 1);
  const std::string& text = buffer.GetLine(line);
  if (text.empty()) {
    return TextPosition{line, 0};
  }
  return TextPosition{line, LastNonBlankColumn(text)};
}

TextPosition NextParagraphBoundary(const core::Buffer& buffer,
                                   TextPosition position, std::size_t count) {
  if (buffer.LineCount() == 0) {
    return position;
  }

  position = ClampPosition(buffer, position);
  count = std::max<std::size_t>(1, count);

  for (std::size_t step = 0; step < count; ++step) {
    bool seen_non_blank = false;
    for (; position.line < buffer.LineCount(); ++position.line) {
      const std::string& line = buffer.GetLine(position.line);
      if (IsBlankLine(line)) {
        seen_non_blank = false;
        continue;
      }

      if (!seen_non_blank) {
        seen_non_blank = true;
        continue;
      }

      std::size_t probe = position.line;
      for (; probe < buffer.LineCount(); ++probe) {
        const std::string& probe_line = buffer.GetLine(probe);
        if (IsBlankLine(probe_line)) {
          break;
        }
      }

      position.line = probe;
      break;
    }

    if (position.line >= buffer.LineCount()) {
      position.line = buffer.LineCount() - 1;
      position.column = buffer.GetLine(position.line).size();
      return position;
    }

    for (; position.line < buffer.LineCount(); ++position.line) {
      const std::string& line = buffer.GetLine(position.line);
      if (!IsBlankLine(line)) {
        break;
      }
    }
  }

  if (position.line >= buffer.LineCount()) {
    position.line = buffer.LineCount() - 1;
    position.column = buffer.GetLine(position.line).size();
    return position;
  }

  const std::string& line = buffer.GetLine(position.line);
  position.column = FirstNonBlankColumn(line);
  return position;
}

TextPosition PreviousParagraphBoundary(const core::Buffer& buffer,
                                       TextPosition position,
                                       std::size_t count) {
  if (buffer.LineCount() == 0) {
    return position;
  }

  position = ClampPosition(buffer, position);
  count = std::max<std::size_t>(1, count);

  auto retreat_to_previous_line = [&]() {
    if (position.line == 0) {
      position.column = 0;
      return false;
    }
    position.line -= 1;
    position.column = 0;
    return true;
  };

  for (std::size_t step = 0; step < count; ++step) {
    bool seen_non_blank = false;

    while (true) {
      const std::string& line = buffer.GetLine(position.line);
      if (IsBlankLine(line)) {
        if (!retreat_to_previous_line()) {
          return TextPosition{0, 0};
        }
        seen_non_blank = false;
        continue;
      }

      if (!seen_non_blank) {
        seen_non_blank = true;
        if (!retreat_to_previous_line()) {
          return TextPosition{0, 0};
        }
        continue;
      }

      break;
    }

    position.line += 1;
    while (position.line < buffer.LineCount()) {
      const std::string& candidate = buffer.GetLine(position.line);
      if (!IsBlankLine(candidate)) {
        break;
      }
      position.line += 1;
    }
  }

  if (position.line >= buffer.LineCount()) {
    position.line = buffer.LineCount() - 1;
    position.column = buffer.GetLine(position.line).size();
    return position;
  }

  const std::string& line = buffer.GetLine(position.line);
  position.column = FirstNonBlankColumn(line);
  return position;
}

std::optional<std::size_t> FindCharForward(const std::string& line,
                                           const FindParams& params) {
  if (line.empty()) {
    return std::nullopt;
  }

  std::size_t start_column = params.start_column;
  if (start_column >= line.size()) {
    start_column = line.empty() ? 0 : line.size() - 1;
  }

  std::size_t effective_count = std::max<std::size_t>(1, params.count);
  std::size_t probe = start_column;

  while (effective_count > 0) {
    if (probe + 1 >= line.size()) {
      return std::nullopt;
    }
    probe = line.find(params.target, probe + 1);
    if (probe == std::string::npos) {
      return std::nullopt;
    }
    if (--effective_count == 0) {
      return probe;
    }
  }

  return std::nullopt;
}

std::optional<std::size_t> FindCharBackward(const std::string& line,
                                            const FindParams& params) {
  if (line.empty()) {
    return std::nullopt;
  }

  std::size_t effective_count = std::max<std::size_t>(1, params.count);
  std::size_t start_column = params.start_column;
  if (start_column >= line.size()) {
    start_column = line.empty() ? 0 : line.size() - 1;
  }

  std::size_t probe = start_column;

  while (effective_count > 0) {
    if (probe == 0) {
      return std::nullopt;
    }
    const std::size_t kSearchFrom = probe - 1;
    probe = line.rfind(params.target, kSearchFrom);
    if (probe == std::string::npos) {
      return std::nullopt;
    }
    if (--effective_count == 0) {
      return probe;
    }
  }

  return std::nullopt;
}

std::optional<FindMotionResult> ResolveFindMotion(const core::Buffer& buffer,
                                                  TextPosition start,
                                                  FindParams params,
                                                  FindOperationKind kind) {
  if (buffer.LineCount() == 0) {
    return std::nullopt;
  }

  start = ClampPosition(buffer, start);
  if (start.line >= buffer.LineCount()) {
    return std::nullopt;
  }

  const std::string& line = buffer.GetLine(start.line);
  if (line.empty()) {
    return std::nullopt;
  }

  const bool kBackward = kind == FindOperationKind::kBackwardTo ||
                         kind == FindOperationKind::kBackwardTill;
  const bool kTill = kind == FindOperationKind::kForwardTill ||
                     kind == FindOperationKind::kBackwardTill;

  params.count = std::max<std::size_t>(1, params.count);
  params.start_column = start.column;

  std::optional<std::size_t> found = kBackward ? FindCharBackward(line, params)
                                               : FindCharForward(line, params);
  if (!found.has_value()) {
    return std::nullopt;
  }

  const std::size_t kMatchedColumn = *found;
  std::size_t cursor_column = kMatchedColumn;

  if (!kBackward) {
    if (kTill) {
      if (kMatchedColumn == 0) {
        return std::nullopt;
      }
      cursor_column = kMatchedColumn - 1;
    }
  } else {
    if (kTill) {
      cursor_column = kMatchedColumn + 1;
      if (cursor_column > line.size()) {
        cursor_column = line.size();
      }
    }
  }

  cursor_column = (std::min)(cursor_column, line.size());

  if (cursor_column == start.column) {
    return std::nullopt;
  }

  FindMotionResult result;
  result.cursor = TextPosition{start.line, cursor_column};
  result.matched_column = kMatchedColumn;
  result.include_target_char = !kTill;
  result.backward = kBackward;
  return result;
}

TextPosition PreviousWordStart(const core::Buffer& buffer,
                               TextPosition position) {
  if (buffer.LineCount() == 0) {
    return position;
  }

  position = ClampPosition(buffer, position);

  auto retreat_line = [&]() {
    if (position.line == 0) {
      position.column = 0;
      return false;
    }
    position.line -= 1;
    position.column = buffer.GetLine(position.line).size();
    return true;
  };

  if (position.column > 0) {
    position.column -= 1;
  } else if (!retreat_line()) {
    return TextPosition{0, 0};
  }

  while (true) {
    const std::string& line = buffer.GetLine(position.line);
    const std::size_t kLineLength = line.size();
    if (kLineLength == 0) {
      if (!retreat_line()) {
        return TextPosition{0, 0};
      }
      continue;
    }

    if (position.column >= kLineLength) {
      position.column = kLineLength - 1;
    }

    const unsigned char kCurrentChar =
        static_cast<unsigned char>(line.at(position.column));
    if (std::isspace(kCurrentChar) != 0) {
      if (position.column == 0) {
        if (!retreat_line()) {
          return TextPosition{0, 0};
        }
      } else {
        position.column -= 1;
      }
      continue;
    }

    const bool kCurrentIsWord = IsWordChar(static_cast<char>(kCurrentChar));
    while (position.column > 0) {
      const unsigned char kPrevChar =
          static_cast<unsigned char>(line.at(position.column - 1));
      if (std::isspace(kPrevChar) != 0) {
        break;
      }
      const bool kPrevIsWord = IsWordChar(static_cast<char>(kPrevChar));
      if (kPrevIsWord != kCurrentIsWord) {
        break;
      }
      position.column -= 1;
    }

    return position;
  }
}

TextPosition WordEndInclusive(const core::Buffer& buffer,
                              TextPosition position) {
  if (buffer.LineCount() == 0) {
    return position;
  }

  position = ClampPosition(buffer, position);

  while (position.line < buffer.LineCount()) {
    const std::string& line = buffer.GetLine(position.line);
    const std::size_t kLineLength = line.size();

    if (position.column >= kLineLength) {
      if (position.line + 1 >= buffer.LineCount()) {
        return TextPosition{position.line, kLineLength};
      }
      position.line += 1;
      position.column = 0;
      continue;
    }

    const unsigned char kCurrentChar =
        static_cast<unsigned char>(line.at(position.column));
    if (std::isspace(kCurrentChar) != 0) {
      position.column += 1;
      continue;
    }

    const bool kInitialIsWord = IsWordChar(static_cast<char>(kCurrentChar));
    std::size_t probe = position.column;
    while (probe < kLineLength) {
      const unsigned char kProbeChar =
          static_cast<unsigned char>(line.at(probe));
      if (std::isspace(kProbeChar) != 0) {
        break;
      }
      const bool kProbeIsWord = IsWordChar(static_cast<char>(kProbeChar));
      if (kProbeIsWord != kInitialIsWord) {
        break;
      }
      probe += 1;
    }

    if (probe == position.column) {
      return position;
    }

    return TextPosition{position.line, probe - 1};
  }

  const std::size_t kLastLineIndex = buffer.LineCount() - 1;
  return TextPosition{kLastLineIndex, buffer.GetLine(kLastLineIndex).size()};
}

TextPosition LineEndPosition(const core::Buffer& buffer, std::size_t line) {
  if (buffer.LineCount() == 0) {
    return TextPosition{};
  }
  if (line >= buffer.LineCount()) {
    line = buffer.LineCount() - 1;
  }
  return TextPosition{line, buffer.GetLine(line).size()};
}

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
      const std::size_t kDeleted = DeleteLineRange(state_.CursorLine(), kLines);
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

  if (kValue == '0' && !has_pending_count_) {
    if (pending_normal_command_ == "d") {
      const std::size_t kLine = state_.CursorLine();
      const Buffer& buffer_view = state_.GetBuffer();
      const std::size_t kColumn =
          (std::min)(state_.CursorColumn(), buffer_view.GetLine(kLine).size());
      pending_normal_command_.clear();
      ResetCount();
      if (kColumn == 0) {
        state_.SetStatus("Already at line start", StatusSeverity::kWarning);
      } else if (DeleteCharacterRange(kLine, 0, kLine, kColumn)) {
        state_.SetCursor(kLine, 0);
        state_.MoveCursorLine(0);
        state_.SetStatus("Deleted to line start", StatusSeverity::kInfo);
      } else {
        state_.SetStatus("Delete failed", StatusSeverity::kWarning);
      }
      return;
    }

    if (pending_normal_command_.empty()) {
      ResetCount();
      const std::size_t kLine = state_.CursorLine();
      state_.SetCursor(kLine, 0);
      state_.MoveCursorLine(0);
      state_.ClearStatus();
      return;
    }
  }

  if (std::isdigit(static_cast<unsigned char>(kValue)) != 0) {
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
    if (pending_normal_command_ == "f" || pending_normal_command_ == "F" ||
        pending_normal_command_ == "t" || pending_normal_command_ == "T") {
      const char kCommand = pending_normal_command_.front();
      pending_normal_command_.clear();
      ApplyFindCommand(kCommand, false, kValue);
      return;
    }

    if (pending_normal_command_.front() == 'd') {
      if (pending_normal_command_.size() == 2) {
        const char kCommand = pending_normal_command_.back();
        pending_normal_command_.clear();
        ApplyFindCommand(kCommand, true, kValue);
        return;
      }

      switch (kValue) {
        case 'd': {
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
        case 'W': {
          pending_normal_command_.clear();
          const Buffer& buffer_view = state_.GetBuffer();
          TextPosition start{state_.CursorLine(), state_.CursorColumn()};
          TextPosition current = start;
          std::size_t requested = ConsumeCountOr(1);
          if (requested == 0) {
            requested = 1;
          }

          std::size_t completed = 0;
          for (; completed < requested; ++completed) {
            TextPosition next = NextBigWordStart(buffer_view, current);
            if (next == current) {
              break;
            }
            current = next;
          }

          if (completed == 0) {
            state_.SetStatus("No WORD ahead", StatusSeverity::kWarning);
            return;
          }

          if (DeleteCharacterRange(start.line, start.column, current.line,
                                   current.column)) {
            state_.SetCursor(start.line, start.column);
            state_.MoveCursorLine(0);
            std::ostringstream message;
            message << "Deleted " << completed << " WORD";
            if (completed != 1) {
              message << 's';
            }
            state_.SetStatus(message.str(), StatusSeverity::kInfo);
          } else {
            state_.SetStatus("Delete failed", StatusSeverity::kWarning);
          }
          return;
        }
        case 'B': {
          pending_normal_command_.clear();
          const Buffer& buffer_view = state_.GetBuffer();
          TextPosition start{state_.CursorLine(), state_.CursorColumn()};
          TextPosition current = start;
          std::size_t requested = ConsumeCountOr(1);
          if (requested == 0) {
            requested = 1;
          }

          std::size_t completed = 0;
          for (; completed < requested; ++completed) {
            TextPosition prev = PreviousBigWordStart(buffer_view, current);
            if (prev == current) {
              break;
            }
            current = prev;
          }

          if (completed == 0) {
            state_.SetStatus("No WORD before", StatusSeverity::kWarning);
          } else if (DeleteCharacterRange(current.line, current.column,
                                          start.line, start.column)) {
            state_.SetCursor(current.line, current.column);
            state_.MoveCursorLine(0);
            std::ostringstream message;
            message << "Deleted " << completed << " WORD";
            if (completed != 1) {
              message << 's';
            }
            state_.SetStatus(message.str(), StatusSeverity::kInfo);
          } else {
            state_.SetStatus("Delete failed", StatusSeverity::kWarning);
          }
          return;
        }
        case 'E': {
          pending_normal_command_.clear();
          const Buffer& buffer_view = state_.GetBuffer();
          TextPosition start{state_.CursorLine(), state_.CursorColumn()};
          TextPosition current = start;
          std::size_t requested = ConsumeCountOr(1);
          if (requested == 0) {
            requested = 1;
          }

          std::size_t completed = 0;
          TextPosition last = start;
          for (; completed < requested; ++completed) {
            TextPosition end = BigWordEndInclusive(buffer_view, current);
            if (end == current && end == last) {
              break;
            }
            last = end;
            current = TextPosition{end.line, end.column + 1};
          }

          if (completed == 0) {
            state_.SetStatus("No WORD ahead", StatusSeverity::kWarning);
          } else {
            const std::string& line_text = buffer_view.GetLine(last.line);
            std::size_t exclusive_column =
                std::min<std::size_t>(last.column + 1, line_text.size());
            if (DeleteCharacterRange(start.line, start.column, last.line,
                                     exclusive_column)) {
              state_.SetCursor(start.line, start.column);
              state_.MoveCursorLine(0);
              std::ostringstream message;
              message << "Deleted " << completed << " WORD";
              if (completed != 1) {
                message << 's';
              }
              state_.SetStatus(message.str(), StatusSeverity::kInfo);
            } else {
              state_.SetStatus("Delete failed", StatusSeverity::kWarning);
            }
          }
          return;
        }
        case 'j': {
          pending_normal_command_.clear();
          const std::size_t kLines =
              std::max<std::size_t>(1, ConsumeCountOr(2));
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
        case 'k': {
          const std::size_t kLines =
              std::max<std::size_t>(1, ConsumeCountOr(2));
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
        case '}': {
          pending_normal_command_.clear();
          const Buffer& buffer_view = state_.GetBuffer();
          if (buffer_view.LineCount() == 0) {
            state_.SetStatus("Buffer empty", StatusSeverity::kWarning);
            return;
          }

          TextPosition start{state_.CursorLine(), state_.CursorColumn()};
          std::size_t count = ConsumeCountOr(1);
          if (count == 0) {
            count = 1;
          }

          TextPosition target =
              NextParagraphBoundary(buffer_view, start, count);
          if (target == start) {
            state_.SetStatus("No paragraph ahead", StatusSeverity::kWarning);
            return;
          }

          if (DeleteCharacterRange(start.line, start.column, target.line,
                                   target.column)) {
            state_.SetCursor(start.line, start.column);
            state_.MoveCursorLine(0);
            std::ostringstream message;
            message << "Deleted to paragraph";
            if (count > 1) {
              message << " (" << count << ')';
            }
            state_.SetStatus(message.str(), StatusSeverity::kInfo);
          } else {
            state_.SetStatus("Delete failed", StatusSeverity::kWarning);
          }
          return;
        }
        case '{': {
          pending_normal_command_.clear();
          const Buffer& buffer_view = state_.GetBuffer();
          if (buffer_view.LineCount() == 0) {
            state_.SetStatus("Buffer empty", StatusSeverity::kWarning);
            return;
          }

          TextPosition start{state_.CursorLine(), state_.CursorColumn()};
          std::size_t count = ConsumeCountOr(1);
          if (count == 0) {
            count = 1;
          }

          TextPosition target =
              PreviousParagraphBoundary(buffer_view, start, count);
          if (target == start) {
            state_.SetStatus("No paragraph before", StatusSeverity::kWarning);
            return;
          }

          if (DeleteCharacterRange(target.line, target.column, start.line,
                                   start.column)) {
            state_.SetCursor(target.line, target.column);
            state_.MoveCursorLine(0);
            std::ostringstream message;
            message << "Deleted to paragraph";
            if (count > 1) {
              message << " (" << count << ')';
            }
            state_.SetStatus(message.str(), StatusSeverity::kInfo);
          } else {
            state_.SetStatus("Delete failed", StatusSeverity::kWarning);
          }
          return;
        }
        case 'w': {
          pending_normal_command_.clear();
          const Buffer& buffer_view = state_.GetBuffer();
          TextPosition start{state_.CursorLine(), state_.CursorColumn()};
          TextPosition current = start;
          std::size_t requested = ConsumeCountOr(1);
          if (requested == 0) {
            requested = 1;
          }

          std::size_t completed = 0;
          for (; completed < requested; ++completed) {
            TextPosition next = NextWordStart(buffer_view, current);
            if (next == current) {
              break;
            }
            current = next;
          }

          if (completed == 0) {
            state_.SetStatus("No word ahead", StatusSeverity::kWarning);
            return;
          }

          if (DeleteCharacterRange(start.line, start.column, current.line,
                                   current.column)) {
            state_.SetCursor(start.line, start.column);
            state_.MoveCursorLine(0);
            std::ostringstream message;
            message << "Deleted " << completed << " word";
            if (completed != 1) {
              message << 's';
            }
            state_.SetStatus(message.str(), StatusSeverity::kInfo);
          } else {
            state_.SetStatus("Delete failed", StatusSeverity::kWarning);
          }
          return;
        }
        case 'b': {
          pending_normal_command_.clear();
          const Buffer& buffer_view = state_.GetBuffer();
          TextPosition start{state_.CursorLine(), state_.CursorColumn()};
          TextPosition current = start;
          std::size_t requested = ConsumeCountOr(1);
          if (requested == 0) {
            requested = 1;
          }

          std::size_t completed = 0;
          for (; completed < requested; ++completed) {
            TextPosition prev = PreviousWordStart(buffer_view, current);
            if (prev == current) {
              break;
            }
            current = prev;
          }

          if (completed == 0) {
            state_.SetStatus("No word before", StatusSeverity::kWarning);
          } else if (DeleteCharacterRange(current.line, current.column,
                                          start.line, start.column)) {
            state_.SetCursor(current.line, current.column);
            state_.MoveCursorLine(0);
            std::ostringstream message;
            message << "Deleted " << completed << " word";
            if (completed != 1) {
              message << 's';
            }
            state_.SetStatus(message.str(), StatusSeverity::kInfo);
          } else {
            state_.SetStatus("Delete failed", StatusSeverity::kWarning);
          }
          return;
        }
        case 'e': {
          pending_normal_command_.clear();
          const Buffer& buffer_view = state_.GetBuffer();
          TextPosition start{state_.CursorLine(), state_.CursorColumn()};
          TextPosition current = start;
          std::size_t requested = ConsumeCountOr(1);
          if (requested == 0) {
            requested = 1;
          }

          std::size_t completed = 0;
          TextPosition last = start;
          for (; completed < requested; ++completed) {
            TextPosition end = WordEndInclusive(buffer_view, current);
            if (end == current && end == last) {
              break;
            }
            last = end;
            current = TextPosition{end.line, end.column + 1};
          }

          if (completed == 0) {
            state_.SetStatus("No word ahead", StatusSeverity::kWarning);
          } else {
            const std::string& line_text = buffer_view.GetLine(last.line);
            std::size_t exclusive_column =
                std::min<std::size_t>(last.column + 1, line_text.size());
            if (DeleteCharacterRange(start.line, start.column, last.line,
                                     exclusive_column)) {
              state_.SetCursor(start.line, start.column);
              state_.MoveCursorLine(0);
              std::ostringstream message;
              message << "Deleted " << completed << " word";
              if (completed != 1) {
                message << 's';
              }
              state_.SetStatus(message.str(), StatusSeverity::kInfo);
            } else {
              state_.SetStatus("Delete failed", StatusSeverity::kWarning);
            }
          }
          return;
        }
        case 'f':
        case 'F':
        case 't':
        case 'T': {
          pending_normal_command_.push_back(kValue);
          std::string status;
          if (has_pending_count_) {
            status = std::to_string(pending_count_);
          }
          status.push_back('d');
          status.push_back(kValue);
          state_.SetStatus(status, StatusSeverity::kInfo);
          return;
        }
        case ';': {
          pending_normal_command_.clear();
          ApplyRepeatFind(false, true);
          return;
        }
        case ',': {
          pending_normal_command_.clear();
          ApplyRepeatFind(true, true);
          return;
        }
        case '$': {
          pending_normal_command_.clear();
          const Buffer& buffer_view = state_.GetBuffer();
          if (buffer_view.LineCount() == 0) {
            state_.SetStatus("Buffer empty", StatusSeverity::kWarning);
            return;
          }

          TextPosition start{state_.CursorLine(), state_.CursorColumn()};
          std::size_t count = ConsumeCountOr(1);
          if (count == 0) {
            count = 1;
          }

          const std::size_t kLastLine = buffer_view.LineCount() - 1;
          std::size_t target_line = start.line;
          if (count > 1) {
            const std::size_t kDelta = count - 1;
            target_line = (std::min)(start.line + kDelta, kLastLine);
          }

          TextPosition end = LineEndPosition(buffer_view, target_line);
          if (start == end) {
            state_.SetStatus("Nothing to delete", StatusSeverity::kWarning);
            return;
          }

          if (DeleteCharacterRange(start.line, start.column, end.line,
                                   end.column)) {
            state_.SetCursor(start.line, start.column);
            state_.MoveCursorLine(0);
            std::ostringstream message;
            message << "Deleted to line end";
            if (target_line != start.line) {
              message << " (" << (target_line - start.line + 1) << " lines)";
            }
            state_.SetStatus(message.str(), StatusSeverity::kInfo);
          } else {
            state_.SetStatus("Delete failed", StatusSeverity::kWarning);
          }
          return;
        }
        default:
          pending_normal_command_.clear();
          state_.SetStatus("d command requires motion",
                           StatusSeverity::kWarning);
          ResetCount();
          return;
      }
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
    case 'b': {
      pending_normal_command_.clear();
      const Buffer& buffer_view = state_.GetBuffer();
      TextPosition current{state_.CursorLine(), state_.CursorColumn()};
      std::size_t requested = ConsumeCountOr(1);
      if (requested == 0) {
        requested = 1;
      }

      std::size_t completed = 0;
      for (; completed < requested; ++completed) {
        TextPosition prev = PreviousWordStart(buffer_view, current);
        if (prev == current) {
          break;
        }
        current = prev;
      }

      if (current == TextPosition{state_.CursorLine(), state_.CursorColumn()}) {
        state_.SetStatus("Start of buffer", StatusSeverity::kWarning);
      } else {
        state_.SetCursor(current.line, current.column);
        state_.MoveCursorLine(0);
        state_.ClearStatus();
      }
      break;
    }
    case 'B': {
      pending_normal_command_.clear();
      const Buffer& buffer_view = state_.GetBuffer();
      TextPosition current{state_.CursorLine(), state_.CursorColumn()};
      std::size_t requested = ConsumeCountOr(1);
      if (requested == 0) {
        requested = 1;
      }

      std::size_t completed = 0;
      for (; completed < requested; ++completed) {
        TextPosition prev = PreviousBigWordStart(buffer_view, current);
        if (prev == current) {
          break;
        }
        current = prev;
      }

      if (current == TextPosition{state_.CursorLine(), state_.CursorColumn()}) {
        state_.SetStatus("Start of buffer", StatusSeverity::kWarning);
      } else {
        state_.SetCursor(current.line, current.column);
        state_.MoveCursorLine(0);
        state_.ClearStatus();
      }
      break;
    }
    case 'e': {
      pending_normal_command_.clear();
      const Buffer& buffer_view = state_.GetBuffer();
      TextPosition current{state_.CursorLine(), state_.CursorColumn()};
      std::size_t requested = ConsumeCountOr(1);
      if (requested == 0) {
        requested = 1;
      }

      TextPosition last = current;
      std::size_t completed = 0;
      for (; completed < requested; ++completed) {
        TextPosition end = WordEndInclusive(
            buffer_view, TextPosition{current.line, current.column});
        if (end == current && end == last) {
          break;
        }
        last = end;
        current = TextPosition{end.line, end.column + 1};
      }

      if (completed == 0) {
        state_.SetStatus("End of buffer", StatusSeverity::kWarning);
      } else {
        std::size_t target_column = last.column;
        const std::string& line_text = buffer_view.GetLine(last.line);
        if (target_column >= line_text.size()) {
          target_column = line_text.empty() ? 0 : line_text.size() - 1;
        }
        state_.SetCursor(last.line, target_column);
        state_.MoveCursorLine(0);
        state_.ClearStatus();
      }
      break;
    }
    case 'E': {
      pending_normal_command_.clear();
      const Buffer& buffer_view = state_.GetBuffer();
      TextPosition current{state_.CursorLine(), state_.CursorColumn()};
      std::size_t requested = ConsumeCountOr(1);
      if (requested == 0) {
        requested = 1;
      }

      TextPosition last = current;
      std::size_t completed = 0;
      for (; completed < requested; ++completed) {
        TextPosition end = BigWordEndInclusive(
            buffer_view, TextPosition{current.line, current.column});
        if (end == current && end == last) {
          break;
        }
        last = end;
        current = TextPosition{end.line, end.column + 1};
      }

      if (completed == 0) {
        state_.SetStatus("End of buffer", StatusSeverity::kWarning);
      } else {
        std::size_t target_column = last.column;
        const std::string& line_text = buffer_view.GetLine(last.line);
        if (target_column >= line_text.size()) {
          target_column = line_text.empty() ? 0 : line_text.size() - 1;
        }
        state_.SetCursor(last.line, target_column);
        state_.MoveCursorLine(0);
        state_.ClearStatus();
      }
      break;
    }
    case 'w': {
      pending_normal_command_.clear();
      const Buffer& buffer_view = state_.GetBuffer();
      TextPosition start{state_.CursorLine(), state_.CursorColumn()};
      TextPosition current = start;
      std::size_t requested = ConsumeCountOr(1);
      if (requested == 0) {
        requested = 1;
      }

      std::size_t completed = 0;
      for (; completed < requested; ++completed) {
        TextPosition next = NextWordStart(buffer_view, current);
        if (next == current) {
          break;
        }
        current = next;
      }

      if (current == start) {
        state_.SetStatus("End of buffer", StatusSeverity::kWarning);
      } else {
        state_.SetCursor(current.line, current.column);
        state_.MoveCursorLine(0);
        state_.ClearStatus();
      }
      break;
    }
    case 'W': {
      pending_normal_command_.clear();
      const Buffer& buffer_view = state_.GetBuffer();
      TextPosition start{state_.CursorLine(), state_.CursorColumn()};
      TextPosition current = start;
      std::size_t requested = ConsumeCountOr(1);
      if (requested == 0) {
        requested = 1;
      }

      std::size_t completed = 0;
      for (; completed < requested; ++completed) {
        TextPosition next = NextBigWordStart(buffer_view, current);
        if (next == current) {
          break;
        }
        current = next;
      }

      if (current == start) {
        state_.SetStatus("End of buffer", StatusSeverity::kWarning);
      } else {
        state_.SetCursor(current.line, current.column);
        state_.MoveCursorLine(0);
        state_.ClearStatus();
      }
      break;
    }
    case '}': {
      pending_normal_command_.clear();
      const Buffer& buffer_view = state_.GetBuffer();
      if (buffer_view.LineCount() == 0) {
        state_.SetStatus("Buffer empty", StatusSeverity::kWarning);
        break;
      }

      TextPosition start{state_.CursorLine(), state_.CursorColumn()};
      std::size_t count = ConsumeCountOr(1);
      if (count == 0) {
        count = 1;
      }

      TextPosition target = NextParagraphBoundary(buffer_view, start, count);
      if (target == start) {
        state_.SetStatus("End of buffer", StatusSeverity::kWarning);
      } else {
        state_.SetCursor(target.line, target.column);
        state_.MoveCursorLine(0);
        state_.ClearStatus();
      }
      break;
    }
    case '{': {
      pending_normal_command_.clear();
      const Buffer& buffer_view = state_.GetBuffer();
      if (buffer_view.LineCount() == 0) {
        state_.SetStatus("Buffer empty", StatusSeverity::kWarning);
        break;
      }

      TextPosition start{state_.CursorLine(), state_.CursorColumn()};
      std::size_t count = ConsumeCountOr(1);
      if (count == 0) {
        count = 1;
      }

      TextPosition target =
          PreviousParagraphBoundary(buffer_view, start, count);
      if (target == start) {
        state_.SetStatus("Start of buffer", StatusSeverity::kWarning);
      } else {
        state_.SetCursor(target.line, target.column);
        state_.MoveCursorLine(0);
        state_.ClearStatus();
      }
      break;
    }
    case '$': {
      pending_normal_command_.clear();
      const Buffer& buffer_view = state_.GetBuffer();
      if (buffer_view.LineCount() == 0) {
        state_.SetStatus("Buffer empty", StatusSeverity::kWarning);
        break;
      }

      std::size_t count = ConsumeCountOr(1);
      if (count == 0) {
        count = 1;
      }

      const std::size_t kLastLine = buffer_view.LineCount() - 1;
      std::size_t target_line = state_.CursorLine();
      if (count > 1) {
        const std::size_t kDelta = count - 1;
        target_line = (std::min)(target_line + kDelta, kLastLine);
      }

      TextPosition target = LineEndPosition(buffer_view, target_line);
      state_.SetCursor(target.line, target.column);
      state_.MoveCursorLine(0);
      state_.ClearStatus();
      break;
    }
    case 'f':
    case 'F':
    case 't':
    case 'T': {
      pending_normal_command_ = std::string(1, kValue);
      std::string status;
      if (has_pending_count_) {
        status = std::to_string(pending_count_);
      }
      status.push_back(kValue);
      state_.SetStatus(status, StatusSeverity::kInfo);
      break;
    }
    case ';': {
      pending_normal_command_.clear();
      ApplyRepeatFind(false, false);
      break;
    }
    case ',': {
      pending_normal_command_.clear();
      ApplyRepeatFind(true, false);
      break;
    }
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

bool EditorApp::ApplyFindCommand(char command, bool is_delete, char target) {
  FindOperationKind kind = FindKindFromCommand(command);
  const Buffer& buffer_view = state_.GetBuffer();
  TextPosition start{state_.CursorLine(), state_.CursorColumn()};
  std::size_t count = ConsumeCountOr(1);
  FindParams params{};
  params.target = target;
  params.count = count;

  auto result = ResolveFindMotion(buffer_view, start, params, kind);
  if (!result.has_value()) {
    state_.SetStatus("Target not found", StatusSeverity::kWarning);
    return false;
  }

  const bool kTill = kind == FindOperationKind::kForwardTill ||
                     kind == FindOperationKind::kBackwardTill;

  if (is_delete) {
    auto& buffer = state_.GetBuffer();
    const std::string& line_text = buffer.GetLine(start.line);
    const std::size_t kLineLength = line_text.size();

    if (!result->backward) {
      std::size_t end_column = result->include_target_char
                                   ? result->matched_column + 1
                                   : result->cursor.column + 1;
      end_column = (std::min)(end_column, kLineLength);
      if (end_column <= start.column) {
        state_.SetStatus("Nothing to delete", StatusSeverity::kWarning);
        return false;
      }
      if (!DeleteCharacterRange(start.line, start.column, start.line,
                                end_column)) {
        state_.SetStatus("Delete failed", StatusSeverity::kWarning);
        return false;
      }
      state_.SetCursor(start.line, start.column);
    } else {
      std::size_t range_start = result->include_target_char
                                    ? result->matched_column
                                    : result->cursor.column;
      if (range_start >= start.column) {
        state_.SetStatus("Nothing to delete", StatusSeverity::kWarning);
        return false;
      }
      std::size_t range_end = start.column + 1;
      if (range_end > kLineLength) {
        range_end = kLineLength;
      }
      if (!DeleteCharacterRange(start.line, range_start, start.line,
                                range_end)) {
        state_.SetStatus("Delete failed", StatusSeverity::kWarning);
        return false;
      }
      state_.SetCursor(start.line, range_start);
    }

    state_.MoveCursorLine(0);
    std::ostringstream message;
    message << "Deleted to '" << target << '\'';
    if (count > 1) {
      message << " (" << count << ')';
    }
    state_.SetStatus(message.str(), StatusSeverity::kInfo);
  } else {
    state_.SetCursor(result->cursor.line, result->cursor.column);
    state_.MoveCursorLine(0);
    state_.ClearStatus();
  }

  has_last_find_ = true;
  last_find_target_ = target;
  last_find_backward_ = result->backward;
  last_find_till_ = kTill;
  return true;
}

bool EditorApp::ApplyRepeatFind(bool reverse_direction, bool is_delete) {
  if (!has_last_find_) {
    state_.SetStatus("No previous find", StatusSeverity::kWarning);
    return false;
  }

  bool backward = last_find_backward_;
  if (reverse_direction) {
    backward = !backward;
  }

  char command = CommandFromState(backward, last_find_till_);
  return ApplyFindCommand(command, is_delete, last_find_target_);
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

bool EditorApp::DeleteCharacterRange(std::size_t start_line,
                                     std::size_t start_column,
                                     std::size_t end_line,
                                     std::size_t end_column) {
  auto& buffer = state_.GetBuffer();
  const Buffer& buffer_const = buffer;
  if (buffer.LineCount() == 0) {
    return false;
  }

  if (start_line > end_line ||
      (start_line == end_line && start_column >= end_column)) {
    return false;
  }

  start_line = (std::min)(start_line, buffer.LineCount() - 1);
  end_line = (std::min)(end_line, buffer.LineCount() - 1);

  const std::string& start_line_text = buffer_const.GetLine(start_line);
  const std::string& end_line_text = buffer_const.GetLine(end_line);

  start_column = (std::min)(start_column, start_line_text.size());
  end_column = (std::min)(end_column, end_line_text.size());

  if (start_line == end_line) {
    if (start_column >= end_column) {
      return false;
    }
    std::string& target = buffer.GetLine(start_line);
    target.erase(start_column, end_column - start_column);
    return true;
  }

  std::string prefix = start_line_text.substr(0, start_column);
  std::string suffix = end_line_text.substr(end_column);

  const std::size_t kLinesToDelete = end_line - start_line;
  for (std::size_t i = 0; i < kLinesToDelete; ++i) {
    buffer.DeleteLine(start_line + 1);
  }

  std::string& merged = buffer.GetLine(start_line);
  merged = std::move(prefix) + std::move(suffix);
  return true;
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