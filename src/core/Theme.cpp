#include "core/Theme.hpp"

namespace core {
Theme DefaultTheme() {
  Theme theme;
  theme.status_info = "\x1b[30;47m";     // black on white
  theme.status_warning = "\x1b[30;43m";  // black on yellow
  theme.status_error = "\x1b[97;41m";    // bright white on red
  theme.reset = "\x1b[0m";
  return theme;
}
}  // namespace core
