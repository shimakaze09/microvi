#pragma once

#include <thread>

#include "core/EditorState.hpp"
#include "core/EventQueue.hpp"
#include "core/InputHandler.hpp"
#include "core/ModeController.hpp"
#include "core/Renderer.hpp"
#include "io/ConsoleKeySource.hpp"

namespace core {
class EditorApp {
 public:
  EditorApp();

  int Run(int argc, char** argv);

 private:
  void LoadFile(int argc, char** argv);
  void Render();
  void HandleEvent(const KeyEvent& event);
  static void ConfigureConsole();
  void StartInputLoop();
  void StopInputLoop();
  void InputLoop(const std::stop_token& token);
  void ProcessPendingEvents();

  EditorState state_;
  ConsoleKeySource key_source_;
  InputHandler command_handler_;
  EventQueue event_queue_;
  ModeController mode_controller_;
  Renderer renderer_;
  std::jthread input_thread_;
};
}  // namespace core