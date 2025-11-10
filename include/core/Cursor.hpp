#pragma once

#include <cstddef>

namespace core {
struct Cursor {
  std::size_t row = 1;
  std::size_t column = 1;
};
}  // namespace core
