#include "io/Terminal.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace core {
auto QueryTerminalSize() -> TerminalSize {
#ifdef _WIN32
  const HANDLE kHandle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (kHandle != nullptr && kHandle != INVALID_HANDLE_VALUE) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(kHandle, &info) != 0) {
      const int kRowsSpan = static_cast<int>(info.srWindow.Bottom) -
                            static_cast<int>(info.srWindow.Top) + 1;
      const int kColumnsSpan = static_cast<int>(info.srWindow.Right) -
                               static_cast<int>(info.srWindow.Left) + 1;
      const std::size_t kRows =
          kRowsSpan > 0 ? static_cast<std::size_t>(kRowsSpan) : 0;
      const std::size_t kColumns =
          kColumnsSpan > 0 ? static_cast<std::size_t>(kColumnsSpan) : 0;
      if (kRows > 0 && kColumns > 0) {
        return TerminalSize{kRows, kColumns};
      }
    }
  }
#else
  winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
    if (ws.ws_row > 0 && ws.ws_col > 0) {
      return TerminalSize{static_cast<std::size_t>(ws.ws_row),
                          static_cast<std::size_t>(ws.ws_col)};
    }
  }
#endif

  return TerminalSize{};
}
}  // namespace core
