set(MICROVI_CORE_SOURCES
  "Buffer.cpp"
  "EventQueue.cpp"
  "EditorState.cpp"
  "EditorApp.cpp"
  "ModeController.cpp"
  "Renderer.cpp"
  "InputHandler.cpp"
  "Registry.cpp"
  "Theme.cpp"
  "../io/ConsoleKeySource.cpp"
  "../io/Terminal.cpp"
)

add_library(microvi_core STATIC
  ${MICROVI_CORE_SOURCES}
)

target_include_directories(microvi_core
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
