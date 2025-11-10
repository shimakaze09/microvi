#include <memory>
#include <string>
#include <utility>

#include "core/InputHandler.hpp"

#include "core/Command.hpp"
#include "core/EditorState.hpp"

namespace core {
void InputHandler::RegisterCommand(std::unique_ptr<Command> command) {
  commands_.push_back(std::move(command));
}

bool InputHandler::Handle(EditorState& state, const std::string& input) {
  for (auto& cmd : commands_) {
    if (cmd->Matches(input)) {
      cmd->Execute(state, input);
      return true;
    }
  }
  return false;
}
}  // namespace core