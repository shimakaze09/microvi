#pragma once

#include <cstddef>
#include <string>

#include "core/ConsoleKeySource.hpp"
#include "core/EditorState.hpp"
#include "core/InputHandler.hpp"

namespace core {
class EditorApp {
 public:
  EditorApp();

  int Run(int argc, char** argv);

 private:
  void LoadFile(int argc, char** argv);
  void Render() const;
  void HandleEvent(const KeyEvent& event);
  void HandleNormalMode(const KeyEvent& event);
  void HandleInsertMode(const KeyEvent& event);
  void HandleCommandMode(const KeyEvent& event);
  bool ExecuteCommandLine(const std::string& line);
  void UpdateScroll(std::size_t content_rows) const;
  void InsertCharacter(char value);
  void InsertNewline();
  void HandleBackspace();
  static void ConfigureConsole();
  void PrepareScreen();
  void RestoreScreen();

  EditorState state_;
  ConsoleKeySource key_source_;
  InputHandler command_handler_;
  std::string command_buffer_;
  std::string pending_normal_command_;
  bool screen_prepared_ = false;
  mutable bool first_render_ = true;
  mutable std::string previous_frame_;
  mutable std::size_t scroll_offset_ = 0;
};
}  // namespace core