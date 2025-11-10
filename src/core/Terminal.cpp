#include "core/Terminal.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace core {
auto QueryTerminalSize() -> TerminalSize {
#ifdef _WIN32
  const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(handle, &info) != 0) {
      const std::size_t kRows = static_cast<std::size_t>(info.srWindow.Bottom -
                                                         info.srWindow.Top + 1);
      const std::size_t kColumns = static_cast<std::size_t>(
          info.srWindow.Right - info.srWindow.Left + 1);
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
