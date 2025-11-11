#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "core/Theme.hpp"

namespace core {
class EditorState;

class Renderer {
 public:
  Renderer();

  void Prepare();
  void Restore();
  void Render(const EditorState& state, std::string_view command_buffer,
              char command_prefix);
  void SetTheme(const Theme& theme);
  const Theme& GetTheme() const noexcept;

 private:
  void UpdateScroll(const EditorState& state, std::size_t content_rows);

  Theme theme_;
  bool prepared_ = false;
  bool first_render_ = true;
  std::string previous_frame_;
  std::size_t scroll_offset_ = 0;
};
}  // namespace core
