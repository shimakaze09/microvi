#include <string>

#include "commands/QuitCommand.hpp"

#include "core/Buffer.hpp"
#include "core/EditorState.hpp"

namespace commands {
bool QuitCommand::Matches(const std::string& input) const {
  return input == ":q" || input == ":q!";
}

void QuitCommand::Execute(core::EditorState& state, const std::string& input) {
  const bool kForce = input == ":q!";
  const bool kDirty = state.GetBuffer().IsDirty();

  if (kDirty && !kForce) {
    state.SetStatus("Unsaved changes. Use :q! to force quit.",
                    core::StatusSeverity::kWarning);
    return;
  }

  state.ClearStatus();
  state.RequestQuit();
}
}  // namespace commands