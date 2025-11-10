#pragma once

#include <cstdint>

namespace core {
enum class KeyCode : std::uint8_t {
  kCharacter,
  kEscape,
  kEnter,
  kBackspace,
  kArrowUp,
  kArrowDown,
  kArrowLeft,
  kArrowRight,
};

struct KeyEvent {
  KeyCode code{};
  char value = '\0';
};

inline KeyEvent MakeCharacterEvent(char value) {
  return KeyEvent{KeyCode::kCharacter, value};
}
}  // namespace core