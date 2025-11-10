#pragma once

#include <string>
#include "../core/Command.hpp"

namespace commands {
class DeleteCommand : public core::Command {
public:
  bool Matches(const std::string& input) const override;
  void Execute(core::EditorState& state, const std::string& input) override;
};
} // namespace commands