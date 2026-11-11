// User textures for image views (sprint 1 L10): PNG/JPEG/GIF/BMP bytes decoded with stb_image and
// uploaded once to the GPU; ImGui draws them by ImTextureID (an SDL_GPUTexture* under the
// SDL_GPU backend, an SDL_Texture* under the simulator's SDL_Renderer fallback). Owned here,
// released at teardown. No caching policy beyond "the caller keeps the handle".
#pragma once

#include <SDL3/SDL.h>
#include <cstddef>
#include <imgui.h>
#include <string>

namespace ke::ui::textures {

struct Texture {
    ImTextureID id = 0;
    int w = 0;
    int h = 0;
    bool valid() const { return id != 0 && w > 0 && h > 0; }
};

// One of the two: the GPU device (desktop, iOS device) or the renderer (iOS simulator).
void init(SDL_GPUDevice* device);
void initRenderer(SDL_Renderer* renderer);
void shutdown();
bool ready();

// Decode encoded image bytes and upload; on failure `err` says why and the result is invalid.
Texture upload(const unsigned char* bytes, std::size_t n, std::string& err);
// Decode black-on-light artwork and keep only its coverage: white pixels with the ink as alpha, to
// be tinted when drawn. A typeset formula, in any theme.
Texture uploadCoverage(const unsigned char* bytes, std::size_t n, std::string& err);
// Upload RGBA8 pixels as they are.
Texture uploadRGBA(const unsigned char* rgba, int w, int h, std::string& err);
void release(Texture& t);

// An image shipped in assets, by file name, loaded once. Through SDL's IO, so a phone reads it too.
void setAssetsDir(const std::string& dir);
const Texture& asset(const std::string& name);
// That image square at the cursor, a text line tall; nothing when it is missing.
void assetIcon(const std::string& name);

} // namespace ke::ui::textures
