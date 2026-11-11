# ADR 0005: SDL_GPU backend for Dear ImGui

Status: accepted, 2026-10-29

## Context
macOS; OpenGL is deprecated; Objective-C++ in tree is undesirable.
## Decision
SDL3 + imgui_impl_sdlgpu3 with SDL_CreateGPUDevice(MSL|METALLIB). Fallback: imgui_impl_sdlrenderer3.
## Consequences
Portable to Vulkan/D3D later with no UI code change. Screensaver hint must be set (user rule).
