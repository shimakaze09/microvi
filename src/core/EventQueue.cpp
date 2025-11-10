#include "core/EventQueue.hpp"

namespace core {
void EventQueue::Push(const KeyEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  events_.push_back(event);
}

std::vector<KeyEvent> EventQueue::ConsumeAll() {
  std::vector<KeyEvent> result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    result.swap(events_);
  }
  return result;
}
}  // namespace core
