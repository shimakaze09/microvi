#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/InputHandler.hpp"
#include "core/KeyEvent.hpp"
#include "core/Registry.hpp"

namespace core {
class EditorState;

class ModeController {
 public:
  ModeController(EditorState& state, InputHandler& command_handler);
  ~ModeController();

  void HandleEvent(const KeyEvent& event);
  std::string_view CommandBuffer() const noexcept;

 private:
  enum class FindCommandAction : std::uint8_t {
    kMove,
    kDelete,
    kYank,
  };

  void HandleNormalMode(const KeyEvent& event);
  void HandleInsertMode(const KeyEvent& event);
  void HandleCommandMode(const KeyEvent& event);
  bool ExecuteCommandLine(const std::string& line);
  void InitializeRegistryBindings();
  bool ExecuteRegisteredBinding(const KeyEvent& event);
  static KeybindingMode ToKeybindingMode(Mode mode) noexcept;
  std::string MakeGesture(const KeyEvent& event) const;
  bool InvokeCommand(const std::string& command_id,
                     const std::unordered_map<std::string, std::string>& args);

  void InsertCharacter(char value);
  void InsertNewline();
  void HandleBackspace();

  bool ApplyFindCommand(char command, FindCommandAction action, char target);
  bool ApplyRepeatFind(bool reverse_direction, FindCommandAction action);
  bool HandlePendingFind(char input, FindCommandAction action);

  bool HandleDeleteOperator(char motion);
  bool HandleYankOperator(char motion);

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

  EditorState& state_;
  InputHandler& command_handler_;
  Registry& registry_;
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
  std::vector<RegistrationHandle> registry_handles_;
};
}  // namespace core
