#include <algorithm>
#include <cstddef>

#include "core/Buffer.hpp"

#include "core/EditorState.hpp"
#include "core/Mode.hpp"

namespace core {
EditorState::EditorState() = default;

Buffer& EditorState::GetBuffer() noexcept {
  return buffer_;
}

const Buffer& EditorState::GetBuffer() const noexcept {
  return buffer_;
}

std::size_t EditorState::CursorLine() const noexcept {
  return cursor_line_;
}

std::size_t EditorState::CursorColumn() const noexcept {
  return cursor_column_;
}

void EditorState::SetCursor(std::size_t line, std::size_t column) {
  cursor_line_ = line;
  cursor_column_ = column;
  ClampCursor();
}

void EditorState::MoveCursorLine(int delta) {
  if (buffer_.LineCount() == 0) {
    cursor_line_ = 0;
    cursor_column_ = 0;
    return;
  }

  const int kMaxLine = static_cast<int>(buffer_.LineCount()) - 1;
  int target = static_cast<int>(cursor_line_) + delta;
  target = std::clamp(target, 0, kMaxLine);
  cursor_line_ = static_cast<std::size_t>(target);
  ClampCursor();
}

void EditorState::MoveCursorColumn(int delta) {
  const auto kLineLength = buffer_.GetLine(cursor_line_).size();
  const int kMaxColumn = static_cast<int>(kLineLength);
  int target = static_cast<int>(cursor_column_) + delta;
  target = std::clamp(target, 0, kMaxColumn);
  cursor_column_ = static_cast<std::size_t>(target);
  ClampCursor();
}

Mode EditorState::CurrentMode() const noexcept {
  return mode_;
}

void EditorState::SetMode(Mode mode) noexcept {
  mode_ = mode;
}

bool EditorState::IsRunning() const noexcept {
  return running_;
}

void EditorState::RequestQuit() noexcept {
  running_ = false;
}

void EditorState::SetStatus(const std::string& message) {
  status_message_ = message;
}

const std::string& EditorState::Status() const noexcept {
  return status_message_;
}

void EditorState::ClampCursor() {
  if (buffer_.LineCount() == 0) {
    cursor_line_ = 0;
    cursor_column_ = 0;
    return;
  }

  if (cursor_line_ >= buffer_.LineCount()) {
    cursor_line_ = buffer_.LineCount() - 1;
  }

  const auto& line = buffer_.GetLine(cursor_line_);
  cursor_column_ = std::min(cursor_column_, line.size());
}
}  // namespace core