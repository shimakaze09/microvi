#pragma once

#include <chrono>

#include "core/KeyEvent.hpp"

#ifndef _WIN32
#include <termios.h>
#endif

namespace core {
class ConsoleKeySource {
 public:
  ConsoleKeySource();
  ~ConsoleKeySource();

  ConsoleKeySource(const ConsoleKeySource&) = delete;
  ConsoleKeySource& operator=(const ConsoleKeySource&) = delete;
  ConsoleKeySource(ConsoleKeySource&&) = delete;
  ConsoleKeySource& operator=(ConsoleKeySource&&) = delete;

  KeyEvent Next();
  bool Poll(KeyEvent& event);

 private:
#ifndef _WIN32
  bool has_original_mode_ = false;
  termios original_{};
  int original_flags_ = -1;
#endif
  int last_code_ = 0;
};
}  // namespace core
