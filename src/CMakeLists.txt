add_subdirectory(core)
add_subdirectory(commands)

set(MICROVI_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include)

add_executable(microvi
  main.cpp
)

target_link_libraries(microvi
  PRIVATE
    microvi_commands
)

target_include_directories(microvi
  PRIVATE
    ${MICROVI_INCLUDE_DIR}
)

install(TARGETS microvi RUNTIME DESTINATION bin)
