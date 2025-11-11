#include "core/ModeController.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>


#include "core/Buffer.hpp"
#include "core/EditorState.hpp"
#include "core/Mode.hpp"

namespace {
constexpr char kCommandPrefix = ':';
constexpr std::size_t kMaxCountValue = 1000000;

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

std::size_t AppendCountDigit(std::size_t current, std::size_t digit) {
  const std::size_t kNext = current * 10 + digit;
  return kNext > kMaxCountValue ? kMaxCountValue : kNext;
}

std::string FormatPendingStatus(std::string_view pending_command,
                                std::size_t prefix_count, bool has_prefix_count,
                                std::size_t motion_count,
                                bool has_motion_count) {
  std::string status;
  if (has_prefix_count && prefix_count > 0) {
    status += std::to_string(prefix_count);
  }
  status.append(pending_command);
  if (has_motion_count && motion_count > 0) {
    status += std::to_string(motion_count);
  }
  return status;
}

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

    const bool kCurrentIsWord = IsWordChar(line.at(position.column));
    while (position.column > 0) {
      const unsigned char kPrevChar =
          static_cast<unsigned char>(line.at(position.column - 1));
      const bool kPrevIsWord = IsWordChar(line.at(position.column - 1));
      if (std::isspace(kPrevChar) != 0 || kPrevIsWord != kCurrentIsWord) {
        break;
      }
      position.column -= 1;
    }

    return position;
  }
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
  const std::size_t kColumn = LastNonBlankColumn(text);
  return TextPosition{line, kColumn};
}

TextPosition NextParagraphStart(const core::Buffer& buffer,
                                TextPosition position) {
  if (buffer.LineCount() == 0) {
    return position;
  }

  position = ClampPosition(buffer, position);

  const std::size_t kTotalLines = buffer.LineCount();
  std::size_t line = position.line;

  bool in_blank = IsBlankLine(buffer.GetLine(line));
  while (line + 1 < kTotalLines) {
    ++line;
    const bool kCurrentBlank = IsBlankLine(buffer.GetLine(line));
    if (!kCurrentBlank && in_blank) {
      return TextPosition{line, FirstNonBlankColumn(buffer.GetLine(line))};
    }
    in_blank = kCurrentBlank;
  }

  return TextPosition{kTotalLines - 1, buffer.GetLine(kTotalLines - 1).size()};
}

TextPosition PreviousParagraphStart(const core::Buffer& buffer,
                                    TextPosition position) {
  if (buffer.LineCount() == 0) {
    return position;
  }

  position = ClampPosition(buffer, position);

  std::size_t line = position.line;
  bool in_blank = IsBlankLine(buffer.GetLine(line));

  while (line > 0) {
    --line;
    const bool kCurrentBlank = IsBlankLine(buffer.GetLine(line));
    if (!kCurrentBlank && in_blank) {
      return TextPosition{line, FirstNonBlankColumn(buffer.GetLine(line))};
    }
    in_blank = kCurrentBlank;
  }

  return TextPosition{0, 0};
}

TextPosition ParagraphEndInclusive(const core::Buffer& buffer,
                                   TextPosition position) {
  if (buffer.LineCount() == 0) {
    return position;
  }

  position = ClampPosition(buffer, position);
  std::size_t line = position.line;
  const std::size_t kTotalLines = buffer.LineCount();

  while (line < kTotalLines) {
    const bool kBlank = IsBlankLine(buffer.GetLine(line));
    if (kBlank) {
      if (line == 0) {
        return TextPosition{0, 0};
      }
      return LastNonBlankPosition(buffer, line - 1);
    }
    if (line + 1 >= kTotalLines) {
      return LastNonBlankPosition(buffer, line);
    }
    line += 1;
  }

  return LastNonBlankPosition(buffer, kTotalLines - 1);
}

bool IsCommandSeparator(char ch) {
  return ch == '|' || ch == ';';
}

std::string TrimCopy(std::string_view value) {
  const auto kFirst = value.find_first_not_of(" \t\r\n");
  if (kFirst == std::string_view::npos) {
    return {};
  }
  const auto kLast = value.find_last_not_of(" \t\r\n");
  return std::string(value.substr(kFirst, kLast - kFirst + 1));
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
ModeController::ModeController(EditorState& state,
                               InputHandler& command_handler)
    : state_(state),
      command_handler_(command_handler),
      registry_(Registry::Instance()) {
  InitializeRegistryBindings();
}

ModeController::~ModeController() {
  for (const RegistrationHandle& handle : registry_handles_) {
    registry_.Unregister(handle);
  }
}

void ModeController::HandleEvent(const KeyEvent& event) {
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

std::string_view ModeController::CommandBuffer() const noexcept {
  return command_buffer_;
}

void ModeController::HandleNormalMode(const KeyEvent& event) {
  if (event.code == KeyCode::kEscape) {
    pending_normal_command_.clear();
    ResetCount();
    state_.ClearStatus();
    return;
  }

  if (ExecuteRegisteredBinding(event)) {
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

  if (kValue == '0' && !has_prefix_count_ && !has_motion_count_) {
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

    if (pending_normal_command_ == "y") {
      const std::size_t kLine = state_.CursorLine();
      const Buffer& buffer_view = state_.GetBuffer();
      const std::size_t kColumn =
          (std::min)(state_.CursorColumn(), buffer_view.GetLine(kLine).size());
      pending_normal_command_.clear();
      ResetCount();
      if (kColumn == 0) {
        state_.SetStatus("Nothing to yank", StatusSeverity::kWarning);
      } else if (CopyCharacterRange(kLine, 0, kLine, kColumn)) {
        state_.SetStatus("Yanked to line start", StatusSeverity::kInfo);
      } else {
        state_.SetStatus("Yank failed", StatusSeverity::kWarning);
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
    const std::size_t kDigit = static_cast<std::size_t>(kValue - '0');
    if (pending_normal_command_.empty()) {
      has_prefix_count_ = true;
      prefix_count_ = AppendCountDigit(prefix_count_, kDigit);
      state_.SetStatus(FormatPendingStatus(pending_normal_command_,
                                           prefix_count_, has_prefix_count_,
                                           motion_count_, has_motion_count_),
                       StatusSeverity::kInfo);
      return;
    }

    has_motion_count_ = true;
    motion_count_ = AppendCountDigit(motion_count_, kDigit);
    state_.SetStatus(FormatPendingStatus(pending_normal_command_, prefix_count_,
                                         has_prefix_count_, motion_count_,
                                         has_motion_count_),
                     StatusSeverity::kInfo);
    return;
  }

  switch (kValue) {
    case 'h': {
      pending_normal_command_.clear();
      const std::size_t kCount = ConsumeCountOr(1);
      state_.MoveCursorColumn(-ToSignedDelta(kCount));
      state_.ClearStatus();
      return;
    }
    case 'j': {
      pending_normal_command_.clear();
      const std::size_t kCount = ConsumeCountOr(1);
      state_.MoveCursorLine(ToSignedDelta(kCount));
      state_.ClearStatus();
      return;
    }
    case 'k': {
      pending_normal_command_.clear();
      const std::size_t kCount = ConsumeCountOr(1);
      state_.MoveCursorLine(-ToSignedDelta(kCount));
      state_.ClearStatus();
      return;
    }
    case 'l': {
      pending_normal_command_.clear();
      const std::size_t kCount = ConsumeCountOr(1);
      state_.MoveCursorColumn(ToSignedDelta(kCount));
      state_.ClearStatus();
      return;
    }
    case 'i': {
      pending_normal_command_.clear();
      ResetCount();
      state_.SetMode(Mode::kInsert);
      state_.SetStatus("-- INSERT --", StatusSeverity::kInfo);
      return;
    }
    case 'a': {
      pending_normal_command_.clear();
      ResetCount();
      state_.MoveCursorColumn(1);
      state_.SetMode(Mode::kInsert);
      state_.SetStatus("-- INSERT --", StatusSeverity::kInfo);
      return;
    }
    case 'A': {
      pending_normal_command_.clear();
      ResetCount();
      const Buffer& buffer_view = state_.GetBuffer();
      const std::size_t kLine = state_.CursorLine();
      const std::size_t kLength = buffer_view.GetLine(kLine).size();
      state_.SetCursor(kLine, kLength);
      state_.MoveCursorLine(0);
      state_.SetMode(Mode::kInsert);
      state_.SetStatus("-- INSERT --", StatusSeverity::kInfo);
      return;
    }
    case 'I': {
      pending_normal_command_.clear();
      ResetCount();
      TextPosition target =
          FirstNonBlankPosition(state_.GetBuffer(), state_.CursorLine());
      state_.SetCursor(target.line, target.column);
      state_.MoveCursorLine(0);
      state_.SetMode(Mode::kInsert);
      state_.SetStatus("-- INSERT --", StatusSeverity::kInfo);
      return;
    }
    case 'o': {
      pending_normal_command_.clear();
      ResetCount();
      InsertNewline();
      state_.SetMode(Mode::kInsert);
      state_.SetStatus("-- INSERT --", StatusSeverity::kInfo);
      return;
    }
    case 'O': {
      pending_normal_command_.clear();
      ResetCount();
      auto& buffer = state_.GetBuffer();
      const std::size_t kLine = state_.CursorLine();
      if (buffer.InsertLine(kLine, "")) {
        state_.SetCursor(kLine, 0);
        state_.MoveCursorLine(0);
      }
      state_.SetMode(Mode::kInsert);
      state_.SetStatus("-- INSERT --", StatusSeverity::kInfo);
      return;
    }
    case ':': {
      pending_normal_command_.clear();
      ResetCount();
      command_buffer_.clear();
      state_.SetMode(Mode::kCommandLine);
      state_.SetStatus("-- COMMAND --", StatusSeverity::kInfo);
      return;
    }
    case 'x': {
      pending_normal_command_.clear();
      const std::size_t kCount = ConsumeCountOr(1);
      const std::size_t kLine = state_.CursorLine();
      const std::size_t kStartColumn = state_.CursorColumn();
      const std::size_t kEndColumn = kStartColumn + kCount;
      if (DeleteCharacterRange(kLine, kStartColumn, kLine, kEndColumn)) {
        state_.SetCursor(kLine, kStartColumn);
        state_.MoveCursorLine(0);
        state_.SetStatus("Deleted characters", StatusSeverity::kInfo);
      } else {
        state_.SetStatus("Delete failed", StatusSeverity::kWarning);
      }
      return;
    }
    case 'd':
    case 'c':
    case 'y':
    case 'g':
    case 'z':
    case 'f':
    case 'F':
    case 't':
    case 'T':
    case 'n':
    case 'N':
    case 'p':
    case 'P':
    case 'J':
    case 'G':
    case 'H':
    case 'M':
    case 'L':
    case 'w':
    case 'W':
    case 'b':
    case 'B':
    case 'e':
    case 'E':
    case 's':
    case 'S':
    case 'u':
    case 'r':
    case 'R':
    case '0':
    case '$':
    case 'm':
    case '`':
    case '\'':
    case '~':
    case '*':
    case '#':
    case '%':
    case '^':
    case '(':
    case ')':
    case '{':
    case '}':
      break;
    default:
      break;
  }

  pending_normal_command_.push_back(kValue);
  state_.SetStatus(
      FormatPendingStatus(pending_normal_command_, prefix_count_,
                          has_prefix_count_, motion_count_, has_motion_count_),
      StatusSeverity::kInfo);

  if (pending_normal_command_.size() == 1) {
    const char kCommand = pending_normal_command_.front();
    switch (kCommand) {
      case 'd':
      case 'c':
      case 'y':
        return;
      case 'p':
      case 'P': {
        pending_normal_command_.clear();
        ResetCount();
        if (!PasteAfterCursor()) {
          state_.SetStatus("Paste failed", StatusSeverity::kWarning);
        }
        return;
      }
      case 'u': {
        pending_normal_command_.clear();
        ResetCount();
        state_.SetStatus("Nothing to undo", StatusSeverity::kWarning);
        return;
      }
      case 'r': {
        pending_normal_command_.clear();
        ResetCount();
        state_.SetStatus("Nothing to redo", StatusSeverity::kWarning);
        return;
      }
      case 'n': {
        pending_normal_command_.clear();
        ResetCount();
        ApplyRepeatFind(false, FindCommandAction::kMove);
        return;
      }
      case 'N': {
        pending_normal_command_.clear();
        ResetCount();
        ApplyRepeatFind(true, FindCommandAction::kMove);
        return;
      }
      case 'f':
      case 'F':
      case 't':
      case 'T':
        return;
      default:
        break;
    }
  }

  const std::string kCommand = pending_normal_command_;
  pending_normal_command_.clear();

  if (kCommand == "dd") {
    const std::size_t kCount = ConsumeCountOr(1);
    const std::size_t kDeleted = DeleteLineRange(state_.CursorLine(), kCount);
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

  if (kCommand == "yy") {
    const std::size_t kCount = ConsumeCountOr(1);
    if (CopyLineRange(state_.CursorLine(), kCount)) {
      state_.SetStatus("Yanked line", StatusSeverity::kInfo);
    } else {
      state_.SetStatus("Yank failed", StatusSeverity::kWarning);
    }
    return;
  }

  if (kCommand == "p" || kCommand == "P") {
    ResetCount();
    if (!PasteAfterCursor()) {
      state_.SetStatus("Paste failed", StatusSeverity::kWarning);
    }
    return;
  }

  if (kCommand == "gg") {
    ResetCount();
    state_.SetCursor(0, 0);
    state_.MoveCursorLine(0);
    state_.ClearStatus();
    return;
  }

  if (kCommand == "G") {
    const auto& buffer = state_.GetBuffer();
    const std::size_t kTarget =
        has_prefix_count_ ? (std::min)(prefix_count_, buffer.LineCount())
                          : buffer.LineCount();
    ResetCount();
    if (kTarget == 0) {
      state_.SetCursor(0, 0);
    } else {
      state_.SetCursor(kTarget - 1, 0);
    }
    state_.MoveCursorLine(0);
    state_.ClearStatus();
    return;
  }

  if (kCommand.size() == 2 &&
      (kCommand.front() == 'f' || kCommand.front() == 'F' ||
       kCommand.front() == 't' || kCommand.front() == 'T')) {
    const char kCommandChar = kCommand.front();
    const char kTarget = kCommand.back();
    if (!ApplyFindCommand(kCommandChar, FindCommandAction::kMove, kTarget)) {
      state_.SetStatus("Find failed", StatusSeverity::kWarning);
    }
    return;
  }

  if (kCommand.size() == 2 && kCommand.front() == 'd') {
    if (!HandleDeleteOperator(kCommand.back())) {
      state_.SetStatus("Delete failed", StatusSeverity::kWarning);
    }
    return;
  }

  if (kCommand.size() == 2 && kCommand.front() == 'y') {
    if (!HandleYankOperator(kCommand.back())) {
      state_.SetStatus("Yank failed", StatusSeverity::kWarning);
    }
    return;
  }

  state_.SetStatus("Unknown command", StatusSeverity::kWarning);
  ResetCount();
}

void ModeController::HandleInsertMode(const KeyEvent& event) {
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

void ModeController::HandleCommandMode(const KeyEvent& event) {
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

void ModeController::InitializeRegistryBindings() {
  // Register the built-in normal-mode commands so they flow through the
  // registry just like external contributions.
  Origin origin{RegistryOriginKind::kCore, "core.mode"};

  auto sanitize_gesture = [](const std::string& gesture) {
    std::string result;
    result.reserve(gesture.size());
    for (unsigned char ch : gesture) {
      if (std::isalnum(static_cast<int>(ch)) != 0) {
        result.push_back(static_cast<char>(ch));
      } else {
        result.push_back('_');
      }
    }
    if (result.empty()) {
      result = "binding";
    }
    return result;
  };

  auto register_normal = [&](const std::string& command_id,
                             const std::string& label,
                             CommandCallable::NativeCallback callback,
                             std::initializer_list<std::string> gestures) {
    CommandRegistration command_registration;
    command_registration.descriptor.id = command_id;
    command_registration.descriptor.label = label;
    command_registration.descriptor.short_description = label;
    command_registration.descriptor.modes = {Mode::kNormal};
    command_registration.descriptor.capabilities = 0;
    command_registration.descriptor.undo_scope = UndoScope::kNone;
    command_registration.callable.native_callback = std::move(callback);
    command_registration.lifetime = RegistrationLifetime::kSession;

    RegistrationResult command_result =
        registry_.RegisterCommand(command_registration, origin);
    if (command_result.status != RegistrationStatus::kRejected &&
        command_result.handle.IsValid()) {
      registry_handles_.push_back(command_result.handle);
    }

    if (command_result.status == RegistrationStatus::kRejected) {
      return;
    }

    for (const std::string& gesture : gestures) {
      KeybindingRegistration binding_registration;
      binding_registration.descriptor.id =
          command_id + ".binding." + sanitize_gesture(gesture);
      binding_registration.descriptor.command_id = command_id;
      binding_registration.descriptor.mode = KeybindingMode::kNormal;
      binding_registration.descriptor.gesture = gesture;
      binding_registration.lifetime = RegistrationLifetime::kSession;

      RegistrationResult binding_result =
          registry_.RegisterKeybinding(binding_registration, origin);
      if (binding_result.status != RegistrationStatus::kRejected &&
          binding_result.handle.IsValid()) {
        registry_handles_.push_back(binding_result.handle);
      }
    }
  };

  register_normal("core.normal.move_down", "Move Down",
                  [this](const CommandInvocation&) {
                    pending_normal_command_.clear();
                    std::size_t count = ConsumeCountOr(1);
                    state_.MoveCursorLine(ToSignedDelta(count));
                    state_.ClearStatus();
                  },
                  {"j", "<Down>"});

  register_normal("core.normal.move_up", "Move Up",
                  [this](const CommandInvocation&) {
                    pending_normal_command_.clear();
                    std::size_t count = ConsumeCountOr(1);
                    state_.MoveCursorLine(-ToSignedDelta(count));
                    state_.ClearStatus();
                  },
                  {"k", "<Up>"});

  register_normal("core.normal.move_left", "Move Left",
                  [this](const CommandInvocation&) {
                    pending_normal_command_.clear();
                    std::size_t count = ConsumeCountOr(1);
                    state_.MoveCursorColumn(-ToSignedDelta(count));
                    state_.ClearStatus();
                  },
                  {"h", "<Left>"});

  register_normal("core.normal.move_right", "Move Right",
                  [this](const CommandInvocation&) {
                    pending_normal_command_.clear();
                    std::size_t count = ConsumeCountOr(1);
                    state_.MoveCursorColumn(ToSignedDelta(count));
                    state_.ClearStatus();
                  },
                  {"l", "<Right>"});

  register_normal("core.normal.enter_insert", "Enter Insert Mode",
                  [this](const CommandInvocation&) {
                    pending_normal_command_.clear();
                    ResetCount();
                    state_.SetMode(Mode::kInsert);
                    state_.SetStatus("-- INSERT --", StatusSeverity::kInfo);
                  },
                  {"i"});

  register_normal("core.normal.append", "Append",
                  [this](const CommandInvocation&) {
                    pending_normal_command_.clear();
                    ResetCount();
                    state_.MoveCursorColumn(1);
                    state_.SetMode(Mode::kInsert);
                    state_.SetStatus("-- INSERT --", StatusSeverity::kInfo);
                  },
                  {"a"});

  register_normal("core.normal.append_line_end", "Append at Line End",
                  [this](const CommandInvocation&) {
                    pending_normal_command_.clear();
                    ResetCount();
                    const Buffer& buffer_view = state_.GetBuffer();
                    std::size_t line = state_.CursorLine();
                    std::size_t length = buffer_view.GetLine(line).size();
                    state_.SetCursor(line, length);
                    state_.MoveCursorLine(0);
                    state_.SetMode(Mode::kInsert);
                    state_.SetStatus("-- INSERT --", StatusSeverity::kInfo);
                  },
                  {"A"});

  register_normal("core.normal.insert_line_start", "Insert at Line Start",
                  [this](const CommandInvocation&) {
                    pending_normal_command_.clear();
                    ResetCount();
                    TextPosition target = FirstNonBlankPosition(
                        state_.GetBuffer(), state_.CursorLine());
                    state_.SetCursor(target.line, target.column);
                    state_.MoveCursorLine(0);
                    state_.SetMode(Mode::kInsert);
                    state_.SetStatus("-- INSERT --", StatusSeverity::kInfo);
                  },
                  {"I"});

  register_normal("core.normal.insert_below", "Insert Below",
                  [this](const CommandInvocation&) {
                    pending_normal_command_.clear();
                    ResetCount();
                    InsertNewline();
                    state_.SetMode(Mode::kInsert);
                    state_.SetStatus("-- INSERT --", StatusSeverity::kInfo);
                  },
                  {"o"});

  register_normal("core.normal.insert_above", "Insert Above",
                  [this](const CommandInvocation&) {
                    pending_normal_command_.clear();
                    ResetCount();
                    auto& buffer = state_.GetBuffer();
                    std::size_t line = state_.CursorLine();
                    if (buffer.InsertLine(line, "")) {
                      state_.SetCursor(line, 0);
                      state_.MoveCursorLine(0);
                    }
                    state_.SetMode(Mode::kInsert);
                    state_.SetStatus("-- INSERT --", StatusSeverity::kInfo);
                  },
                  {"O"});
}

bool ModeController::ExecuteRegisteredBinding(const KeyEvent& event) {
  if (!pending_normal_command_.empty()) {
    return false;
  }

  std::string gesture = MakeGesture(event);
  if (gesture.empty()) {
    return false;
  }

  KeybindingMode mode = ToKeybindingMode(state_.CurrentMode());
  auto binding = registry_.ResolveKeybinding(mode, gesture);
  if (!binding.has_value()) {
    binding = registry_.ResolveKeybinding(KeybindingMode::kAny, gesture);
    if (!binding.has_value()) {
      return false;
    }
  }

  const KeybindingRecord& record = *binding;
  return InvokeCommand(record.descriptor.command_id,
                       record.descriptor.arguments);
}

KeybindingMode ModeController::ToKeybindingMode(Mode mode) noexcept {
  switch (mode) {
    case Mode::kNormal:
      return KeybindingMode::kNormal;
    case Mode::kInsert:
      return KeybindingMode::kInsert;
    case Mode::kCommandLine:
      return KeybindingMode::kCommand;
    case Mode::kVisual:
      return KeybindingMode::kVisual;
  }
  return KeybindingMode::kAny;
}

std::string ModeController::MakeGesture(const KeyEvent& event) const {
  switch (event.code) {
    case KeyCode::kCharacter:
      if (event.value != '\0') {
        return std::string(1, event.value);
      }
      return {};
    case KeyCode::kEnter:
      return "<Enter>";
    case KeyCode::kEscape:
      return "<Esc>";
    case KeyCode::kBackspace:
      return "<Backspace>";
    case KeyCode::kArrowUp:
      return "<Up>";
    case KeyCode::kArrowDown:
      return "<Down>";
    case KeyCode::kArrowLeft:
      return "<Left>";
    case KeyCode::kArrowRight:
      return "<Right>";
    default:
      return {};
  }
}

bool ModeController::InvokeCommand(
    const std::string& command_id,
    const std::unordered_map<std::string, std::string>& args) {
  auto command = registry_.FindCommand(command_id, true);
  if (!command.has_value()) {
    state_.SetStatus("Command not found", StatusSeverity::kWarning);
    return false;
  }

  const CommandCallable& callable = command->callable;
  if (callable.native_callback) {
    CommandInvocation invocation;
    invocation.command_id = command_id;
    invocation.arguments = args;
    callable.native_callback(invocation);
    return true;
  }

  state_.SetStatus("Command not executable", StatusSeverity::kWarning);
  return false;
}

void ModeController::InsertCharacter(char value) {
  auto& buffer = state_.GetBuffer();
  const std::size_t kLine = state_.CursorLine();
  const std::size_t kColumn = state_.CursorColumn();

  if (buffer.InsertChar(kLine, kColumn, value)) {
    state_.SetCursor(kLine, kColumn + 1);
  }
}

void ModeController::InsertNewline() {
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

void ModeController::HandleBackspace() {
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

bool ModeController::ApplyFindCommand(char command, FindCommandAction action,
                                      char target) {
  const Buffer& buffer = state_.GetBuffer();
  if (buffer.LineCount() == 0) {
    return false;
  }

  const std::size_t kCount = ConsumeCountOr(1);
  const std::size_t kLine = state_.CursorLine();
  const std::size_t kColumn = state_.CursorColumn();
  const std::string& line = buffer.GetLine(kLine);

  if (line.empty()) {
    state_.SetStatus("Line empty", StatusSeverity::kWarning);
    return false;
  }

  std::optional<FindMotionResult> result;
  const FindOperationKind kKind = FindKindFromCommand(command);
  const bool kBackward = kKind == FindOperationKind::kBackwardTo ||
                         kKind == FindOperationKind::kBackwardTill;
  const bool kTill = kKind == FindOperationKind::kForwardTill ||
                     kKind == FindOperationKind::kBackwardTill;

  std::size_t count = kCount;
  std::size_t position = kColumn;

  while (count > 0) {
    if (kBackward) {
      if (position == 0) {
        state_.SetStatus("Target not found", StatusSeverity::kWarning);
        ResetCount();
        return false;
      }
      position -= 1;
      for (; position < line.size(); --position) {
        if (line.at(position) == target) {
          result = FindMotionResult{
              .cursor = TextPosition{kLine, position},
              .matched_column = position,
              .include_target_char = !kTill,
              .backward = true,
          };
          break;
        }
        if (position == 0) {
          break;
        }
      }
    } else {
      if (position + 1 >= line.size()) {
        state_.SetStatus("Target not found", StatusSeverity::kWarning);
        ResetCount();
        return false;
      }
      position += 1;
      for (; position < line.size(); ++position) {
        if (line.at(position) == target) {
          result = FindMotionResult{
              .cursor = TextPosition{kLine, position},
              .matched_column = position,
              .include_target_char = !kTill,
              .backward = false,
          };
          break;
        }
      }
    }

    if (!result.has_value()) {
      state_.SetStatus("Target not found", StatusSeverity::kWarning);
      ResetCount();
      return false;
    }

    position = result->matched_column;
    count -= 1;
  }

  if (!result.has_value()) {
    state_.SetStatus("Target not found", StatusSeverity::kWarning);
    ResetCount();
    return false;
  }

  auto apply_motion = [&](const FindMotionResult& motion) {
    const bool kInclude = motion.include_target_char;
    const std::size_t kMatchColumn = motion.matched_column;
    const std::size_t kCursorColumn =
        kInclude ? kMatchColumn
                 : (motion.backward ? kMatchColumn + 1 : kMatchColumn - 1);
    state_.SetCursor(motion.cursor.line, kCursorColumn);
    state_.MoveCursorLine(0);
  };

  switch (action) {
    case FindCommandAction::kMove:
      apply_motion(*result);
      break;
    case FindCommandAction::kDelete:
    case FindCommandAction::kYank: {
      const std::size_t kStartColumn =
          (std::min)(kColumn, result->matched_column);
      const std::size_t kEndColumn =
          (std::max)(kColumn, result->matched_column) + 1;
      if (action == FindCommandAction::kDelete) {
        if (!DeleteCharacterRange(kLine, kStartColumn, kLine, kEndColumn)) {
          state_.SetStatus("Delete failed", StatusSeverity::kWarning);
          return false;
        }
        state_.SetCursor(kLine, kStartColumn);
        state_.MoveCursorLine(0);
      } else {
        if (!CopyCharacterRange(kLine, kStartColumn, kLine, kEndColumn)) {
          state_.SetStatus("Yank failed", StatusSeverity::kWarning);
          return false;
        }
        apply_motion(*result);
      }
      break;
    }
  }

  has_last_find_ = true;
  last_find_target_ = target;
  last_find_backward_ = result->backward;
  last_find_till_ = kTill;
  return true;
}

bool ModeController::ApplyRepeatFind(bool reverse_direction,
                                     FindCommandAction action) {
  if (!has_last_find_) {
    state_.SetStatus("No previous find", StatusSeverity::kWarning);
    return false;
  }

  bool backward = last_find_backward_;
  if (reverse_direction) {
    backward = !backward;
  }

  char command = CommandFromState(backward, last_find_till_);
  return ApplyFindCommand(command, action, last_find_target_);
}

bool ModeController::HandlePendingFind(char input, FindCommandAction action) {
  if (pending_normal_command_.empty()) {
    return false;
  }
  char command = pending_normal_command_.front();
  pending_normal_command_.clear();
  ResetCount();
  return ApplyFindCommand(command, action, input);
}

bool ModeController::HandleDeleteOperator(char motion) {
  ResetCount();
  switch (motion) {
    case 'd': {
      const std::size_t kCount = ConsumeCountOr(1);
      const std::size_t kDeleted = DeleteLineRange(state_.CursorLine(), kCount);
      if (kDeleted == 0) {
        return false;
      }
      state_.MoveCursorLine(0);
      std::ostringstream message;
      message << "Deleted " << kDeleted << " line";
      if (kDeleted != 1) {
        message << 's';
      }
      state_.SetStatus(message.str(), StatusSeverity::kInfo);
      return true;
    }
    case 'w':
    case 'W':
    case 'b':
    case 'B':
    case 'e':
    case 'E': {
      const std::size_t kCount = ConsumeCountOr(1);
      const Buffer& buffer = state_.GetBuffer();
      TextPosition start{state_.CursorLine(), state_.CursorColumn()};
      TextPosition end = start;

      auto advance_word = [&](TextPosition position) {
        switch (motion) {
          case 'w':
            return NextWordStart(buffer, position);
          case 'W':
            return NextBigWordStart(buffer, position);
          case 'e':
            return WordEndInclusive(buffer, position);
          case 'E':
            return BigWordEndInclusive(buffer, position);
          case 'b':
            return PreviousWordStart(buffer, position);
          case 'B':
            return PreviousBigWordStart(buffer, position);
          default:
            return position;
        }
      };

      for (std::size_t i = 0; i < kCount; ++i) {
        end = advance_word(end);
      }

      if (motion == 'e' || motion == 'E') {
        end.column += 1;
      }

      if (!DeleteCharacterRange(start.line, start.column, end.line,
                                end.column)) {
        return false;
      }
      state_.SetCursor(start.line, start.column);
      state_.MoveCursorLine(0);
      return true;
    }
    default:
      return false;
  }
}

bool ModeController::HandleYankOperator(char motion) {
  ResetCount();
  switch (motion) {
    case 'y':
      return CopyLineRange(state_.CursorLine(), ConsumeCountOr(1));
    default:
      return false;
  }
}

void ModeController::ResetCount() noexcept {
  prefix_count_ = 0;
  motion_count_ = 0;
  has_prefix_count_ = false;
  has_motion_count_ = false;
}

std::size_t ModeController::ConsumeCountOr(std::size_t fallback) noexcept {
  const bool kHasPrefix = has_prefix_count_ && prefix_count_ > 0;
  const bool kHasMotion = has_motion_count_ && motion_count_ > 0;

  std::size_t result = fallback;
  if (kHasMotion) {
    result = motion_count_;
    if (kHasPrefix) {
      const std::size_t kProduct = prefix_count_ * result;
      result = (std::min)(kProduct, kMaxCountValue);
    }
  } else if (kHasPrefix) {
    result = prefix_count_;
  }

  ResetCount();
  return result;
}

bool ModeController::CopyLineRange(std::size_t start_line,
                                   std::size_t line_count) {
  const Buffer& buffer = state_.GetBuffer();
  if (buffer.LineCount() == 0 || start_line >= buffer.LineCount() ||
      line_count == 0) {
    return false;
  }

  const std::size_t kAvailable = buffer.LineCount() - start_line;
  line_count = (std::min)(line_count, kAvailable);
  if (line_count == 0) {
    return false;
  }

  yank_buffer_.clear();
  yank_buffer_.reserve(line_count);
  for (std::size_t i = 0; i < line_count; ++i) {
    yank_buffer_.push_back(buffer.GetLine(start_line + i));
  }
  yank_linewise_ = true;
  return true;
}

bool ModeController::CopyCharacterRange(std::size_t start_line,
                                        std::size_t start_column,
                                        std::size_t end_line,
                                        std::size_t end_column) {
  const Buffer& buffer = state_.GetBuffer();
  if (buffer.LineCount() == 0) {
    return false;
  }

  if (start_line > end_line ||
      (start_line == end_line && start_column >= end_column)) {
    return false;
  }

  start_line = (std::min)(start_line, buffer.LineCount() - 1);
  end_line = (std::min)(end_line, buffer.LineCount() - 1);

  const std::string& start_text = buffer.GetLine(start_line);
  const std::string& end_text = buffer.GetLine(end_line);

  start_column = (std::min)(start_column, start_text.size());
  end_column = (std::min)(end_column, end_text.size());

  if (start_line == end_line) {
    if (start_column >= end_column) {
      return false;
    }
    yank_buffer_.assign(
        1, start_text.substr(start_column, end_column - start_column));
  } else {
    yank_buffer_.clear();
    yank_buffer_.reserve(end_line - start_line + 1);
    yank_buffer_.push_back(start_text.substr(start_column));
    for (std::size_t line = start_line + 1; line < end_line; ++line) {
      yank_buffer_.push_back(buffer.GetLine(line));
    }
    yank_buffer_.push_back(end_text.substr(0, end_column));
  }

  yank_linewise_ = false;
  return true;
}

bool ModeController::PasteAfterCursor() {
  if (!HasYank()) {
    state_.SetStatus("Nothing to paste", StatusSeverity::kWarning);
    return false;
  }

  auto& buffer = state_.GetBuffer();
  if (buffer.LineCount() == 0) {
    buffer.InsertLine(0, "");
  }

  TextPosition cursor{state_.CursorLine(), state_.CursorColumn()};
  cursor = ClampPosition(buffer, cursor);

  if (yank_linewise_) {
    const std::size_t kInsertLine = cursor.line + 1;
    for (std::size_t i = 0; i < yank_buffer_.size(); ++i) {
      if (!buffer.InsertLine(kInsertLine + i, yank_buffer_[i])) {
        state_.SetStatus("Paste failed", StatusSeverity::kWarning);
        return false;
      }
    }

    const std::size_t kFirstInserted =
        (std::min)(kInsertLine, buffer.LineCount() - 1);
    const std::string& first_line = buffer.GetLine(kFirstInserted);
    const std::size_t kColumn = FirstNonBlankColumn(first_line);
    state_.SetCursor(kFirstInserted, kColumn);
    state_.MoveCursorLine(0);
    return true;
  }

  std::size_t line = cursor.line;
  std::size_t column = cursor.column;
  std::string& current = buffer.GetLine(line);
  const std::size_t kInsertColumn = (std::min)(column + 1, current.size());
  std::string prefix = current.substr(0, kInsertColumn);
  std::string suffix = current.substr(kInsertColumn);
  current = prefix + yank_buffer_.front();

  if (yank_buffer_.size() == 1) {
    current += suffix;
    const std::size_t kInserted = yank_buffer_.front().size();
    const std::size_t kCursorColumn =
        kInserted == 0 ? prefix.size() : prefix.size() + kInserted - 1;
    state_.SetCursor(line, kCursorColumn);
    state_.MoveCursorLine(0);
    return true;
  }

  for (std::size_t i = 1; i < yank_buffer_.size(); ++i) {
    const std::size_t kInsertAt = line + i;
    if (!buffer.InsertLine(kInsertAt, yank_buffer_[i])) {
      state_.SetStatus("Paste failed", StatusSeverity::kWarning);
      return false;
    }
  }

  const std::size_t kLastInsertedLine = line + yank_buffer_.size() - 1;
  std::string& last_line = buffer.GetLine(kLastInsertedLine);
  last_line += suffix;
  const std::size_t kCursorColumn =
      yank_buffer_.back().empty() ? 0 : yank_buffer_.back().size() - 1;
  state_.SetCursor(kLastInsertedLine, kCursorColumn);
  state_.MoveCursorLine(0);
  return true;
}

bool ModeController::HasYank() const noexcept {
  return !yank_buffer_.empty();
}

std::size_t ModeController::DeleteLineRange(std::size_t start_line,
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

bool ModeController::DeleteCharacterRange(std::size_t start_line,
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

bool ModeController::ExecuteCommandLine(const std::string& line) {
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