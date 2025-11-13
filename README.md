# microvi

A minimal, modern text editor written in C++20, inspired by vi/vim.

## Overview

microvi is a lightweight console-based text editor that implements a modal editing interface similar to vi/vim. Built with modern C++ practices, it provides a clean separation between core editing functionality, command processing, and I/O handling.

## Features

- **Modal Editing**: Supports different editing modes (Normal, Insert, etc.)
- **Command System**: Extensible command architecture
- **Buffer Management**: Efficient text buffer handling
- **Console Integration**: Native Windows console support
- **Modern C++**: Written in C++20 with modern best practices

## Architecture

The project is organized into several key components:

- **Core**: Editor state, event handling, rendering, and mode control
- **Commands**: Command implementations (Write, Quit, Delete, etc.)
- **I/O**: Console input/output handling
- **Buffer**: Text buffer management with cursor support

## Prerequisites

- **CMake**: Version 3.20 or higher
- **C++20 Compiler**: LLVM/Clang (recommended and tested)
- **Operating System**: Windows (currently)

### LLVM/Clang Setup

This project has been built and tested using the LLVM toolchain. While other compilers with C++20 support may work, they have not been tested.

To install LLVM on Windows:

1. Download LLVM from [https://llvm.org/](https://llvm.org/)
2. Install and add LLVM to your system PATH
3. Ensure `clang++` is accessible from the command line

## Building

### Basic Build

```powershell
# Clone the repository
git clone https://github.com/shimakaze09/microvi.git
cd microvi

# Create build directory
mkdir build
cd build

# Configure with CMake (using LLVM)
cmake .. -G "Unix Makefiles" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++

# Build
cmake --build .
```

### Build Output

After a successful build, the executable will be located at:

- `build/src/microvi.exe` (Windows)

## Running

```powershell
# Run without a file (create new buffer)
.\build\src\microvi.exe

# Run with a file
.\build\src\microvi.exe path\to\file.txt
```

## Usage

microvi follows vi/vim-style modal editing:

- **Normal Mode**: Default mode for navigation and commands
- **Insert Mode**: For inserting text
- **Command Mode**: For executing commands (`:w`, `:q`, etc.)

### Basic Commands

- `:w` - Write (save) the current buffer
- `:q` - Quit the editor
- `:wq` - Write and quit
- `i` - Enter insert mode (implementation may vary)
- `ESC` - Return to normal mode

## Project Structure

```
microvi/
├── CMakeLists.txt          # Root CMake configuration
├── LICENSE                 # MIT License
├── README.md              # This file
├── build/                 # Build artifacts (generated)
├── include/               # Public headers
│   ├── commands/          # Command interface headers
│   ├── core/             # Core editor headers
│   └── io/               # I/O handling headers
└── src/                  # Source files
    ├── main.cpp          # Application entry point
    ├── commands/         # Command implementations
    ├── core/            # Core editor implementation
    └── io/              # I/O implementation
```

## Development

### Compiler Requirements

- C++20 standard support
- Standard library features: `<thread>`, `<stop_token>`, `<memory>`, etc.

### Build System

The project uses CMake with the following configuration:

- **CMake Version**: 3.20+
- **C++ Standard**: C++20 (required)
- **Exported Compile Commands**: Enabled for IDE integration

### Code Style

- Modern C++ idioms (RAII, smart pointers, etc.)
- Namespace organization (`core`, `commands`)
- Header-only interfaces where appropriate

## Known Limitations

- **Platform**: Currently Windows-only (uses Windows Console API)
- **Build Toolchain**: Only tested with LLVM/Clang
- **Testing**: Other compilers (MSVC, GCC) have not been tested

## Troubleshooting

### CMake Configuration Issues

If CMake fails to find the compiler:

```powershell
# Explicitly specify the compiler paths
cmake .. -G "Unix Makefiles" `
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang.exe" `
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang++.exe"
```

### Build Errors

**C++20 Features Not Available**: Ensure you're using a recent version of LLVM (14+)

## Contributing

Contributions are welcome! Please feel free to submit issues or pull requests.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Inspired by the classic vi/vim text editors
- Built with modern C++ practices and standards
