#pragma once

#include <cstdint>

namespace core {
enum class Mode : std::uint8_t {
  kNormal,
  kInsert,
  kCommandLine,
  kVisual,
};
}  // namespace core