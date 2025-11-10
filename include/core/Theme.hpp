#pragma once

#include <string>

namespace core {
struct Theme {
  std::string status_info;
  std::string status_warning;
  std::string status_error;
  std::string reset;
};

Theme DefaultTheme();
}  // namespace core
