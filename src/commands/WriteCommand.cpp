#include <sstream>
#include <string>

#include "commands/WriteCommand.hpp"

#include "core/Buffer.hpp"
#include "core/EditorState.hpp"

namespace {
std::string TrimLeadingWhitespace(const std::string& value) {
  const auto kPos = value.find_first_not_of(" \t");
  return kPos == std::string::npos ? std::string{} : value.substr(kPos);
}
}  // namespace

namespace commands {
bool WriteCommand::Matches(const std::string& input) const {
  return input.starts_with(":w");
}

void WriteCommand::Execute(core::EditorState& state, const std::string& input) {
  const auto kArgument =
      TrimLeadingWhitespace(input.size() > 2 ? input.substr(2) : std::string{});
  auto& buffer = state.GetBuffer();
  const std::string kTargetPath =
      kArgument.empty() ? buffer.FilePath() : kArgument;

  if (kTargetPath.empty()) {
    state.SetStatus("No file specified for write");
    return;
  }

  if (!buffer.SaveToFile(kTargetPath)) {
    state.SetStatus("Failed to write file");
    return;
  }

  std::ostringstream message;
  message << "Wrote " << buffer.LineCount() << " lines";
  state.SetStatus(message.str());
}
}  // namespace commands