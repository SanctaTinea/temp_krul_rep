# Krul host tools

This directory contains generic native examples for the shared protocol stack.

- krul_host_server demonstrates Surge, Krul, and transport integration.
- Board-specific emulators belong to their board repositories.

Build the tool through the top-level Krul CMake project.

    cmake --preset PC_Debug
    cmake --build --preset PC_Debug
    .\build\PC_Debug\Tools\krul_host_server.exe
