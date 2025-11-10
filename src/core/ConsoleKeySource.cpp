#include "core/ConsoleKeySource.hpp"
#include "core/KeyEvent.hpp"

#ifdef _WIN32
#include <conio.h>
#else
#include <unistd.h>

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
  if (tcgetattr(STDIN_FILENO, &m_original) == 0) {
    termios raw = m_original;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
    raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
      m_has_original_mode_ = true;
    }
  }
#endif
}

ConsoleKeySource::~ConsoleKeySource() {
#ifndef _WIN32
  if (m_has_original_mode_) {
    tcsetattr(STDIN_FILENO, TCSANOW, &m_original);
  }
#endif
}

KeyEvent ConsoleKeySource::Next() {
#ifdef _WIN32
  const int kCode = _getch();
  last_code_ = kCode;
  if (kCode == kPrefixZero || kCode == kPrefixExtended) {
    const int kExtended = _getch();
    last_code_ = kExtended;
    return TranslateExtended(kExtended);
  }
  return TranslateChar(kCode);
#else
  unsigned char ch = 0;
  for (;;) {
    const ssize_t count = ::read(STDIN_FILENO, &ch, 1);
    if (count > 0) {
      break;
    }
    if (count < 0 && errno != EINTR) {
      return key_event{.code = key_code::kEscape};
    }
  }

  m_last_code_ = ch;

  if (ch == kEscapeChar) {
    unsigned char seq = 0;
    const ssize_t first = ::read(STDIN_FILENO, &seq, 1);
    if (first <= 0) {
      return key_event{.code = key_code::kEscape};
    }

    if (seq == kEscapeBracket) {
      unsigned char final = 0;
      const ssize_t second = ::read(STDIN_FILENO, &final, 1);
      if (second <= 0) {
        return key_event{.code = key_code::kEscape};
      }

      m_last_code_ = final;
      switch (final) {
        case kArrowUpSeq:
          return key_event{.code = key_code::kArrowUp};
        case kArrowDownSeq:
          return key_event{.code = key_code::kArrowDown};
        case kArrowLeftSeq:
          return key_event{.code = key_code::kArrowLeft};
        case kArrowRightSeq:
          return key_event{.code = key_code::kArrowRight};
        default:
          return key_event{.code = key_code::kEscape};
      }
    }

    return key_event{.code = key_code::kEscape};
  }

  return TranslateChar(static_cast<int>(ch));
#endif
}
}  // namespace core