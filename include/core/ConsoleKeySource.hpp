#pragma once

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

private:
#ifndef _WIN32
  bool has_original_mode_ = false;
  termios original{};
#endif
  int last_code_ = 0;
};
} // namespace core