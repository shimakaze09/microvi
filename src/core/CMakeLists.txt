set(MICROVI_CORE_SOURCES
  "Buffer.cpp"
  "ConsoleKeySource.cpp"
  "EditorState.cpp"
  "EditorApp.cpp"
  "InputHandler.cpp"
  "Theme.cpp"
  "Terminal.cpp"
)

add_library(microvi_core STATIC
  ${MICROVI_CORE_SOURCES}
)

target_include_directories(microvi_core
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
