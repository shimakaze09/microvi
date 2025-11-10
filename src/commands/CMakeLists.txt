set(MICROVI_COMMAND_SOURCES
  "DeleteCommand.cpp"
  "QuitCommand.cpp"
  "WriteCommand.cpp"
)

add_library(microvi_commands STATIC
  ${MICROVI_COMMAND_SOURCES}
)

target_link_libraries(microvi_commands
  PUBLIC
    microvi_core
)

target_include_directories(microvi_commands
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
