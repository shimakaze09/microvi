#include <cctype>
#include <cstddef>
#include <sstream>
#include <string>

#include "commands/DeleteCommand.hpp"

#include "core/Buffer.hpp"
#include "core/EditorState.hpp"

namespace {
int ParseLineArgument(const std::string& input) {
  std::string digits;
  for (const char kChr : input) {
    if (std::isdigit(static_cast<unsigned char>(kChr)) != 0) {
      digits.push_back(kChr);
    }
  }

  if (digits.empty()) {
    return -1;
  }

  try {
    return std::stoi(digits);
  } catch (...) {
    return -1;
  }
}
}  // namespace

namespace commands {
bool DeleteCommand::Matches(const std::string& input) const {
  return input.starts_with(":d");
}

void DeleteCommand::Execute(core::EditorState& state,
                            const std::string& input) {
  const int kParsed = ParseLineArgument(input.substr(2));
  std::size_t target_line = state.CursorLine();

  if (kParsed > 0) {
    if (static_cast<std::size_t>(kParsed) == 0) {
      state.SetStatus("Invalid line number", core::StatusSeverity::kWarning);
      return;
    }
    target_line = static_cast<std::size_t>(kParsed - 1);
  }

  auto& buffer = state.GetBuffer();
  if (target_line >= buffer.LineCount()) {
    state.SetStatus("Line out of range", core::StatusSeverity::kWarning);
    return;
  }

  buffer.DeleteLine(target_line);
  state.MoveCursorLine(0);

  std::ostringstream message;
  message << "Deleted line " << target_line + 1;
  state.SetStatus(message.str(), core::StatusSeverity::kInfo);
}
}  // namespace commands