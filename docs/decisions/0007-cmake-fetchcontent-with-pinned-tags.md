# ADR 0007: CMake FetchContent with pinned tags

Status: accepted, 2026-10-29

## Context
The listener vendors Dear ImGui (docking), IXWebSocket, nlohmann/json, readerwriterqueue, Catch2.
## Decision
cmake/Dependencies.cmake owns every pin; FETCHCONTENT_BASE_DIR=listener/third_party/_deps (git-ignored). No submodules.
## Consequences
Clean checkout builds with network once; presets set CMAKE_PREFIX_PATH=/opt/homebrew and ignore /opt/local to avoid mixing MacPorts SDL2/FreeType.
