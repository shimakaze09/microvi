#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

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
  enum class FindCommandAction : std::uint8_t {
    kMove,
    kDelete,
    kYank,
  };
  bool ApplyFindCommand(char command, FindCommandAction action, char target);
  bool ApplyRepeatFind(bool reverse_direction, FindCommandAction action);
  bool HandlePendingFind(char input, FindCommandAction action);
  bool HandleDeleteOperator(char motion);
  bool HandleYankOperator(char motion);
  static void ConfigureConsole();
  void PrepareScreen();
  void RestoreScreen();
  void StartInputLoop();
  void StopInputLoop();
  void InputLoop(const std::stop_token& token);
  void ProcessPendingEvents();
  void ResetCount() noexcept;
  std::size_t ConsumeCountOr(std::size_t fallback) noexcept;
  bool CopyLineRange(std::size_t start_line, std::size_t line_count);
  bool CopyCharacterRange(std::size_t start_line, std::size_t start_column,
                          std::size_t end_line, std::size_t end_column);
  bool PasteAfterCursor();
  bool HasYank() const noexcept;
  std::size_t DeleteLineRange(std::size_t start_line, std::size_t line_count);
  bool DeleteCharacterRange(std::size_t start_line, std::size_t start_column,
                            std::size_t end_line, std::size_t end_column);

  EditorState state_;
  ConsoleKeySource key_source_;
  InputHandler command_handler_;
  EventQueue event_queue_;
  std::string command_buffer_;
  std::string pending_normal_command_;
  char last_find_target_ = 0;
  bool has_last_find_ = false;
  bool last_find_backward_ = false;
  bool last_find_till_ = false;
  std::size_t prefix_count_ = 0;
  std::size_t motion_count_ = 0;
  bool has_prefix_count_ = false;
  bool has_motion_count_ = false;
  std::vector<std::string> yank_buffer_;
  bool yank_linewise_ = false;
  bool screen_prepared_ = false;
  mutable bool first_render_ = true;
  mutable std::string previous_frame_;
  mutable std::size_t scroll_offset_ = 0;
  Theme theme_ = DefaultTheme();
  std::jthread input_thread_;
};
}  // namespace core