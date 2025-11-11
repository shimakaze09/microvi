#pragma once

#include <cstddef>

namespace core {
struct TerminalSize {
  std::size_t rows = 24;
  std::size_t columns = 80;
};

auto QueryTerminalSize() -> TerminalSize;
}  // namespace core
