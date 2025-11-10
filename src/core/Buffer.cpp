#include <cstddef>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/Buffer.hpp"

namespace {
constexpr char kLineSeparator = '\n';
}  // namespace

namespace core {
Buffer::Buffer() {
  lines_.emplace_back("");
}

bool Buffer::LoadFromFile(const std::string& file_path) {
  std::ifstream input(file_path);
  if (!input.is_open()) {
    return false;
  }

  lines_.clear();
  for (std::string line; std::getline(input, line);) {
    lines_.push_back(std::move(line));
  }

  if (lines_.empty()) {
    lines_.emplace_back("");
  }

  file_path_ = file_path;
  dirty_ = false;
  return true;
}

bool Buffer::SaveToFile(const std::string& file_path) {
  const std::string kPath = file_path.empty() ? file_path_ : file_path;
  if (kPath.empty()) {
    return false;
  }

  std::ofstream output(kPath, std::ios::trunc);
  if (!output.is_open()) {
    return false;
  }

  for (std::size_t i = 0; i < lines_.size(); ++i) {
    output << lines_[i];
    if (i + 1 < lines_.size()) {
      output << kLineSeparator;
    }
  }

  file_path_ = kPath;
  dirty_ = false;
  return true;
}

bool Buffer::InsertChar(std::size_t line, std::size_t column, char value) {
  if (line >= lines_.size()) {
    return false;
  }

  auto& current = lines_.at(line);
  if (column > current.size()) {
    return false;
  }

  current.insert(current.begin() + static_cast<std::ptrdiff_t>(column), value);
  dirty_ = true;
  return true;
}

bool Buffer::DeleteChar(std::size_t line, std::size_t column) {
  if (line >= lines_.size()) {
    return false;
  }

  auto& current = lines_.at(line);
  if (column == 0 || column > current.size()) {
    return false;
  }

  current.erase(current.begin() + static_cast<std::ptrdiff_t>(column - 1));
  dirty_ = true;
  return true;
}

bool Buffer::InsertLine(std::size_t line_index, const std::string& line) {
  if (line_index > lines_.size()) {
    return false;
  }

  lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(line_index),
                  line);
  dirty_ = true;
  return true;
}

bool Buffer::DeleteLine(std::size_t line_index) {
  if (line_index >= lines_.size()) {
    return false;
  }

  lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(line_index));
  if (lines_.empty()) {
    lines_.emplace_back("");
  }

  dirty_ = true;
  return true;
}

std::size_t Buffer::LineCount() const noexcept {
  return lines_.size();
}

const std::string& Buffer::GetLine(std::size_t line_index) const {
  if (line_index >= lines_.size()) {
    throw std::out_of_range("line index out of range");
  }
  return lines_.at(line_index);
}

std::string& Buffer::GetLine(std::size_t line_index) {
  if (line_index >= lines_.size()) {
    throw std::out_of_range("line index out of range");
  }
  dirty_ = true;
  return lines_.at(line_index);
}

const std::string& Buffer::FilePath() const noexcept {
  return file_path_;
}

void Buffer::SetFilePath(const std::string& file_path) {
  file_path_ = file_path;
}

bool Buffer::IsDirty() const noexcept {
  return dirty_;
}

void Buffer::MarkDirty(bool dirty) noexcept {
  dirty_ = dirty;
}
}  // namespace core