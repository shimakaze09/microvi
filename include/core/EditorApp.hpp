#pragma once

#include <cstddef>
#include <string>
#include <thread>

#include "core/ConsoleKeySource.hpp"
#include "core/EditorState.hpp"
#include "core/EventQueue.hpp"
#include "core/InputHandler.hpp"
#include "core/Theme.hpp"

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
  void StartInputLoop();
  void StopInputLoop();
  void InputLoop(const std::stop_token& token);
  void ProcessPendingEvents();
  void ResetCount() noexcept;
  std::size_t ConsumeCountOr(std::size_t fallback) noexcept;

  EditorState state_;
  ConsoleKeySource key_source_;
  InputHandler command_handler_;
  EventQueue event_queue_;
  std::string command_buffer_;
  std::string pending_normal_command_;
  std::size_t pending_count_ = 0;
  bool has_pending_count_ = false;
  bool screen_prepared_ = false;
  mutable bool first_render_ = true;
  mutable std::string previous_frame_;
  mutable std::size_t scroll_offset_ = 0;
  Theme theme_ = DefaultTheme();
  std::jthread input_thread_;
};
}  // namespace core