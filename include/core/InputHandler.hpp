#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Command.hpp"

namespace core {
class InputHandler {
 public:
  void RegisterCommand(std::unique_ptr<Command> command);
  bool Handle(EditorState& state, const std::string& input);

 private:
  std::vector<std::unique_ptr<Command>> commands_;
};
}  // namespace core