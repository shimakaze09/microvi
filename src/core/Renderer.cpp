#include "core/Renderer.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "core/Buffer.hpp"
#include "core/Cursor.hpp"
#include "core/EditorState.hpp"
#include "core/Mode.hpp"
#include "io/Terminal.hpp"

namespace {
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
}  // namespace

namespace core {
Renderer::Renderer() : theme_(DefaultTheme()) {}

void Renderer::Prepare() {
  if (prepared_) {
    return;
  }

  previous_frame_.clear();
  first_render_ = true;
  prepared_ = true;
}

void Renderer::Restore() {
  if (!prepared_) {
    return;
  }

  std::cout << "\x1b[?25h\x1b[0m\x1b[2J\x1b[H" << std::flush;
  prepared_ = false;
  first_render_ = true;
  previous_frame_.clear();
  scroll_offset_ = 0;
}

void Renderer::Render(const EditorState& state, std::string_view command_buffer,
                      char command_prefix) {
  if (!prepared_) {
    Prepare();
  }

  const TerminalSize kSize = QueryTerminalSize();
  const std::size_t kTotalRows = std::max<std::size_t>(kSize.rows, 3);
  const std::size_t kTotalColumns = kSize.columns;

  const std::size_t kInfoRows = 2;
  const std::size_t kContentRows =
      kTotalRows > kInfoRows ? kTotalRows - kInfoRows : 0;

  UpdateScroll(state, kContentRows);

  const Buffer& buffer = state.GetBuffer();
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
      const bool kIsCursorLine = kLineIndex == state.CursorLine();
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

  const StatusSeverity kSeverity = state.StatusLevel();
  const bool kHighlightStatus = IsHighlightSeverity(kSeverity);

  if (kHighlightStatus) {
    std::string highlight_text = FitToWidth(state.Status(), kTotalColumns);
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
    status_stream << ModeLabel(state.CurrentMode()) << ' ' << kFileLabel;
    if (buffer.IsDirty()) {
      status_stream << " [+]";
    }
    status_stream << "  Ln " << (state.CursorLine() + 1) << ", Col "
                  << (state.CursorColumn() + 1) << "  Lines " << kTotalLines;
    frame << FitToWidth(status_stream.str(), kTotalColumns) << "\x1b[K" << '\n';
  }

  std::string message_line;
  if (state.CurrentMode() == Mode::kCommandLine) {
    message_line = std::string(1, command_prefix) + std::string(command_buffer);
  } else if (kSeverity == StatusSeverity::kInfo) {
    message_line = state.Status();
  }
  frame << FitToWidth(message_line, kTotalColumns) << "\x1b[K";

  frame << "\x1b[J";

  Cursor cursor;
  if (state.CurrentMode() == Mode::kCommandLine) {
    cursor.row = kContentRows + 2;
    cursor.column = 1 + 1 + command_buffer.size();
  } else {
    if (kContentRows == 0) {
      cursor.row = 1;
    } else {
      const std::size_t kRelativeLine =
          state.CursorLine() < scroll_offset_
              ? 0
              : state.CursorLine() - scroll_offset_;
      cursor.row = std::min<std::size_t>(kRelativeLine + 1, kContentRows);
    }
    cursor.column = kPrefixWidth + state.CursorColumn() + 1;
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

void Renderer::SetTheme(const Theme& theme) {
  theme_ = theme;
}

const Theme& Renderer::GetTheme() const noexcept {
  return theme_;
}

void Renderer::UpdateScroll(const EditorState& state,
                            std::size_t content_rows) {
  if (content_rows == 0) {
    scroll_offset_ = 0;
    return;
  }

  const Buffer& buffer = state.GetBuffer();
  const std::size_t kTotalLines = buffer.LineCount();

  if (kTotalLines == 0) {
    scroll_offset_ = 0;
    return;
  }

  if (scroll_offset_ >= kTotalLines) {
    scroll_offset_ = kTotalLines - 1;
  }

  const std::size_t kCursorLine = state.CursorLine();
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
