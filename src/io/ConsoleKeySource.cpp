#include "io/ConsoleKeySource.hpp"

#include <thread>

#include "core/KeyEvent.hpp"

#ifdef _WIN32
#include <conio.h>
#else
#include <unistd.h>

#include <fcntl.h>
#include <cerrno>

#endif

namespace core {
namespace {
constexpr int kEscapeCode = 27;
constexpr int kBackspaceCode = '\b';
constexpr int kDeleteCode = 127;

KeyEvent TranslateChar(int code) {
  switch (code) {
    case '\n':
    case '\r':
      return KeyEvent{.code = KeyCode::kEnter};
    case kEscapeCode:
      return KeyEvent{.code = KeyCode::kEscape};
    case kBackspaceCode:
    case kDeleteCode:
      return KeyEvent{.code = KeyCode::kBackspace};
    default:
      return KeyEvent{.code = KeyCode::kCharacter,
                      .value = static_cast<char>(code)};
  }
}

#ifdef _WIN32
constexpr int kPrefixZero = 0;
constexpr int kPrefixExtended = 0xE0;
constexpr int kArrowUp = 72;
constexpr int kArrowDown = 80;
constexpr int kArrowLeft = 75;
constexpr int kArrowRight = 77;

KeyEvent TranslateExtended(int code) {
  switch (code) {
    case kArrowUp:
      return KeyEvent{.code = KeyCode::kArrowUp};
    case kArrowDown:
      return KeyEvent{.code = KeyCode::kArrowDown};
    case kArrowLeft:
      return KeyEvent{.code = KeyCode::kArrowLeft};
    case kArrowRight:
      return KeyEvent{.code = KeyCode::kArrowRight};
    default:
      return KeyEvent{.code = KeyCode::kCharacter,
                      .value = static_cast<char>(code)};
  }
}
#else
constexpr unsigned char kEscapeChar = static_cast<unsigned char>(kEscapeCode);
constexpr unsigned char kEscapeBracket = '[';
constexpr unsigned char kArrowUpSeq = 'A';
constexpr unsigned char kArrowDownSeq = 'B';
constexpr unsigned char kArrowRightSeq = 'C';
constexpr unsigned char kArrowLeftSeq = 'D';
#endif
}  // namespace

ConsoleKeySource::ConsoleKeySource() {
#ifndef _WIN32
  if (tcgetattr(STDIN_FILENO, &original_) == 0) {
    termios raw = original_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
    raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
      has_original_mode_ = true;
    }
  }

  original_flags_ = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (original_flags_ != -1) {
    fcntl(STDIN_FILENO, F_SETFL, original_flags_ | O_NONBLOCK);
  }
#endif
}

ConsoleKeySource::~ConsoleKeySource() {
#ifndef _WIN32
  if (has_original_mode_) {
    tcsetattr(STDIN_FILENO, TCSANOW, &original_);
  }
  if (original_flags_ != -1) {
    fcntl(STDIN_FILENO, F_SETFL, original_flags_);
  }
#endif
}

KeyEvent ConsoleKeySource::Next() {
  KeyEvent event{};
  while (!Poll(event)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return event;
}

bool ConsoleKeySource::Poll(KeyEvent& event) {
#ifdef _WIN32
  if (_kbhit() == 0) {
    return false;
  }

  const int kCode = _getch();
  last_code_ = kCode;
  if (kCode == kPrefixZero || kCode == kPrefixExtended) {
    const int kExtended = _getch();
    last_code_ = kExtended;
    event = TranslateExtended(kExtended);
  } else {
    event = TranslateChar(kCode);
  }
  return true;
#else
  unsigned char ch = 0;
  const ssize_t count = ::read(STDIN_FILENO, &ch, 1);
  if (count <= 0) {
    if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
        errno != EINTR) {
      event = KeyEvent{.code = KeyCode::kEscape};
      return true;
    }
    return false;
  }

  last_code_ = ch;

  if (ch == kEscapeChar) {
    unsigned char seq = 0;
    const ssize_t first = ::read(STDIN_FILENO, &seq, 1);
    if (first <= 0) {
      event = KeyEvent{.code = KeyCode::kEscape};
      return true;
    }

    if (seq == kEscapeBracket) {
      unsigned char final = 0;
      const ssize_t second = ::read(STDIN_FILENO, &final, 1);
      if (second <= 0) {
        event = KeyEvent{.code = KeyCode::kEscape};
        return true;
      }

      last_code_ = final;
      switch (final) {
        case kArrowUpSeq:
          event = KeyEvent{.code = KeyCode::kArrowUp};
          return true;
        case kArrowDownSeq:
          event = KeyEvent{.code = KeyCode::kArrowDown};
          return true;
        case kArrowLeftSeq:
          event = KeyEvent{.code = KeyCode::kArrowLeft};
          return true;
        case kArrowRightSeq:
          event = KeyEvent{.code = KeyCode::kArrowRight};
          return true;
        default:
          event = KeyEvent{.code = KeyCode::kEscape};
          return true;
      }
    }

    event = KeyEvent{.code = KeyCode::kEscape};
    return true;
  }

  event = TranslateChar(static_cast<int>(ch));
  return true;
#endif
}
}  // namespace core
