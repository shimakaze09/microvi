#pragma once

#include <string>

namespace core {
class EditorState;

class Command {
public:
  Command() = default;
  Command(const Command&) = default;
  Command(Command&&) = delete;
  Command& operator=(const Command&) = default;
  Command& operator=(Command&&) = delete;
  virtual ~Command() = default;

  virtual bool Matches(const std::string& input) const = 0;
  virtual void Execute(EditorState& state, const std::string& input) = 0;
};
} // namespace core