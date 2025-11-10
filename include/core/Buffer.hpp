#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace core {
class Buffer {
 public:
  Buffer();

  bool LoadFromFile(const std::string& file_path);
  bool SaveToFile(const std::string& file_path);

  bool InsertChar(std::size_t line, std::size_t column, char value);
  bool DeleteChar(std::size_t line, std::size_t column);
  bool InsertLine(std::size_t line_index, const std::string& line);
  bool DeleteLine(std::size_t line_index);

  std::size_t LineCount() const noexcept;
  const std::string& GetLine(std::size_t line_index) const;
  std::string& GetLine(std::size_t line_index);

  const std::string& FilePath() const noexcept;
  void SetFilePath(const std::string& file_path);

  bool IsDirty() const noexcept;
  void MarkDirty(bool dirty) noexcept;

 private:
  std::vector<std::string> lines_;
  std::string file_path_;
  bool dirty_ = false;
};
}  // namespace core