#include "core/EditorApp.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "commands/DeleteCommand.hpp"
#include "commands/QuitCommand.hpp"
#include "commands/WriteCommand.hpp"

namespace {
constexpr char kCommandPrefix = ':';
}  // namespace

namespace core {

EditorApp::EditorApp() : mode_controller_(state_, command_handler_) {
  ConfigureConsole();
  command_handler_.RegisterCommand(std::make_unique<commands::WriteCommand>());
  command_handler_.RegisterCommand(std::make_unique<commands::QuitCommand>());
  command_handler_.RegisterCommand(std::make_unique<commands::DeleteCommand>());
}

int EditorApp::Run(int argc, char** argv) {
  renderer_.Prepare();
  LoadFile(argc, argv);
  StartInputLoop();
  Render();

  constexpr auto kFrameDuration = std::chrono::milliseconds(16);

  while (state_.IsRunning()) {
    const auto kFrameStart = std::chrono::steady_clock::now();
    ProcessPendingEvents();
    if (!state_.IsRunning()) {
      break;
    }

    Render();

    const auto kElapsed = std::chrono::steady_clock::now() - kFrameStart;
    if (kElapsed < kFrameDuration) {
      std::this_thread::sleep_for(kFrameDuration - kElapsed);
    }
  }

  StopInputLoop();
  renderer_.Restore();
  return 0;
}

void EditorApp::LoadFile(int argc, char** argv) {
  auto& buffer = state_.GetBuffer();
  const std::span<char*> kArguments(argv, static_cast<std::size_t>(argc));

  if (kArguments.size() <= 1) {
    state_.SetStatus("New Buffer", StatusSeverity::kInfo);
    return;
  }

  const char* path_cstr = kArguments[1];
  const std::string kPath =
      path_cstr != nullptr ? std::string(path_cstr) : std::string{};

  if (kPath.empty()) {
    state_.SetStatus("New Buffer", StatusSeverity::kInfo);
    return;
  }

  if (!buffer.LoadFromFile(kPath)) {
    std::cerr << "Failed to load file: " << kPath << '\n';
    buffer.SetFilePath(kPath);
    state_.SetStatus("New file", StatusSeverity::kInfo);
  } else {
    state_.SetStatus("Loaded file", StatusSeverity::kInfo);
  }
}

void EditorApp::Render() {
  renderer_.Render(state_, mode_controller_.CommandBuffer(), kCommandPrefix);
}

void EditorApp::HandleEvent(const KeyEvent& event) {
  mode_controller_.HandleEvent(event);
}

void EditorApp::ConfigureConsole() {
#ifdef _WIN32
  HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle == INVALID_HANDLE_VALUE) {
    return;
  }

  DWORD mode = 0;
  if (GetConsoleMode(handle, &mode) == 0) {
    return;
  }

  if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
    SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }
#endif
}

void EditorApp::StartInputLoop() {
  StopInputLoop();
  input_thread_ =
      std::jthread([this](const std::stop_token& token) { InputLoop(token); });
}

void EditorApp::StopInputLoop() {
  if (input_thread_.joinable()) {
    input_thread_.request_stop();
    input_thread_.join();
  }
}

void EditorApp::InputLoop(const std::stop_token& token) {
  while (!token.stop_requested()) {
    KeyEvent event{};
    if (key_source_.Poll(event)) {
      event_queue_.Push(event);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
}

void EditorApp::ProcessPendingEvents() {
  auto events = event_queue_.ConsumeAll();
  for (const KeyEvent& event : events) {
    HandleEvent(event);
    if (!state_.IsRunning()) {
      break;
    }
  }
}

}  // namespace core
