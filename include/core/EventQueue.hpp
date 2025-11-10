#pragma once

#include <mutex>
#include <vector>

#include "core/KeyEvent.hpp"

namespace core {
class EventQueue {
 public:
  void Push(const KeyEvent& event);
  std::vector<KeyEvent> ConsumeAll();

 private:
  std::mutex mutex_;
  std::vector<KeyEvent> events_;
};
}  // namespace core
