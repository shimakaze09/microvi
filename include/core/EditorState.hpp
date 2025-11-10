#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "Buffer.hpp"
#include "Mode.hpp"

namespace core {
enum class StatusSeverity : std::uint8_t {
  kNone,
  kInfo,
  kWarning,
  kError,
};

class EditorState {
 public:
  EditorState();

  Buffer& GetBuffer() noexcept;
  const Buffer& GetBuffer() const noexcept;

  std::size_t CursorLine() const noexcept;
  std::size_t CursorColumn() const noexcept;
  void SetCursor(std::size_t line, std::size_t column);
  void MoveCursorLine(int delta);
  void MoveCursorColumn(int delta);

  Mode CurrentMode() const noexcept;
  bool IsRunning() const noexcept;
  void SetMode(Mode mode) noexcept;
  void RequestQuit() noexcept;

  void SetStatus(const std::string& message,
                 StatusSeverity severity = StatusSeverity::kInfo);
  void ClearStatus() noexcept;
  const std::string& Status() const noexcept;
  StatusSeverity StatusLevel() const noexcept;

 private:
  void ClampCursor();

  Buffer buffer_;
  std::size_t cursor_line_ = 0;
  std::size_t cursor_column_ = 0;
  Mode mode_ = Mode::kNormal;
  bool running_ = true;
  std::string status_message_;
  StatusSeverity status_severity_ = StatusSeverity::kNone;
};
}  // namespace core