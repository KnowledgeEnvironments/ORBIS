#include "ui/Textures.hpp"

#include <algorithm>
#include <map>

#include "util/Log.hpp"

#include <cstring>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_FAILURE_USERMSG
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wcast-align"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
#pragma clang diagnostic ignored "-Wcomma"
#pragma clang diagnostic ignored "-Wextra-semi-stmt"
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#pragma clang diagnostic ignored "-Wtautological-type-limit-compare"
#pragma clang diagnostic ignored "-Wswitch-default"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace ke::ui::textures {

namespace {

SDL_GPUDevice* g_device = nullptr;
SDL_Renderer* g_renderer = nullptr;
std::vector<ImTextureID> g_live;

void forget(ImTextureID id) {
    for (std::size_t i = 0; i < g_live.size(); ++i) {
        if (g_live[i] == id) {
            g_live[i] = g_live.back();
            g_live.pop_back();
            return;
        }
    }
}

} // namespace

void init(SDL_GPUDevice* device) {
    g_device = device;
    g_renderer = nullptr;
}

void initRenderer(SDL_Renderer* renderer) {
    g_renderer = renderer;
    g_device = nullptr;
}

bool ready() {
    return g_device != nullptr || g_renderer != nullptr;
}

void shutdown() {
    std::vector<ImTextureID> live;
    live.swap(g_live);
    for (ImTextureID id : live) {
        Texture t;
        t.id = id;
        t.w = t.h = 1;
        release(t);
    }
    g_device = nullptr;
    g_renderer = nullptr;
}

Texture uploadCoverage(const unsigned char* bytes, std::size_t n, std::string& err) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(bytes, static_cast<int>(n), &w, &h, &comp, 4);
    if (px == nullptr) {
        err = stbi_failure_reason();
        return {};
    }
    for (int i = 0; i < w * h; ++i) {
        unsigned char* p = px + i * 4;
        const int light = (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8;
        const int ink = std::min<int>(p[3], 255 - light);
        p[0] = p[1] = p[2] = 255;
        p[3] = static_cast<unsigned char>(ink);
    }
    Texture t = uploadRGBA(px, w, h, err);
    stbi_image_free(px);
    return t;
}

Texture upload(const unsigned char* bytes, std::size_t n, std::string& err) {
    Texture t;
    if (bytes == nullptr || n == 0) {
        err = "no bytes";
        return t;
    }
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(bytes, static_cast<int>(n), &w, &h, &comp, 4);
    if (px == nullptr) {
        err = stbi_failure_reason() ? stbi_failure_reason() : "decode failed";
        return t;
    }
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        err = "image too large: " + std::to_string(w) + "x" + std::to_string(h);
        stbi_image_free(px);
        return t;
    }
    t = uploadRGBA(px, w, h, err);
    stbi_image_free(px);
    return t;
}

Texture uploadRGBA(const unsigned char* rgba, int w, int h, std::string& err) {
    Texture t;
    const std::size_t size = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
    if (g_renderer != nullptr) {
        SDL_Texture* tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
        if (tex == nullptr) {
            err = SDL_GetError();
            return t;
        }
        SDL_UpdateTexture(tex, nullptr, rgba, w * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        t.id = reinterpret_cast<ImTextureID>(tex);
        t.w = w;
        t.h = h;
        g_live.push_back(t.id);
        return t;
    }
    if (g_device == nullptr) {
        err = "textures not initialised";
        return t;
    }
    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width = static_cast<Uint32>(w);
    tci.height = static_cast<Uint32>(h);
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    SDL_GPUTexture* tex = SDL_CreateGPUTexture(g_device, &tci);
    if (tex == nullptr) {
        err = SDL_GetError();
        return t;
    }
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = static_cast<Uint32>(size);
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g_device, &tbi);
    if (tb == nullptr) {
        err = SDL_GetError();
        SDL_ReleaseGPUTexture(g_device, tex);
        return t;
    }
    void* mapped = SDL_MapGPUTransferBuffer(g_device, tb, false);
    if (mapped == nullptr) {
        err = SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(g_device, tb);
        SDL_ReleaseGPUTexture(g_device, tex);
        return t;
    }
    std::memcpy(mapped, rgba, size);
    SDL_UnmapGPUTransferBuffer(g_device, tb);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_device);
    if (cmd == nullptr) {
        err = SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(g_device, tb);
        SDL_ReleaseGPUTexture(g_device, tex);
        return t;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;
    src.offset = 0;
    src.pixels_per_row = static_cast<Uint32>(w);
    src.rows_per_layer = static_cast<Uint32>(h);
    SDL_GPUTextureRegion dst{};
    dst.texture = tex;
    dst.w = static_cast<Uint32>(w);
    dst.h = static_cast<Uint32>(h);
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(g_device, tb);
    t.id = reinterpret_cast<ImTextureID>(tex);
    t.w = w;
    t.h = h;
    g_live.push_back(t.id);
    return t;
}

void release(Texture& t) {
    if (t.id == 0) {
        return;
    }
    forget(t.id);
    if (g_renderer != nullptr) {
        SDL_DestroyTexture(reinterpret_cast<SDL_Texture*>(t.id));
    } else if (g_device != nullptr) {
        SDL_ReleaseGPUTexture(g_device, reinterpret_cast<SDL_GPUTexture*>(t.id));
    }
    t = Texture{};
}

namespace {

std::string g_assets;
std::map<std::string, Texture> g_loaded;

} // namespace

void setAssetsDir(const std::string& dir) { g_assets = dir; }

const Texture& asset(const std::string& name) {
    auto [it, fresh] = g_loaded.try_emplace(name);
    if (fresh && ready()) {
        std::size_t size = 0;
        if (void* bytes = SDL_LoadFile((g_assets + "/" + name).c_str(), &size)) {
            std::string err;
            it->second = upload(static_cast<const unsigned char*>(bytes), size, err);
            SDL_free(bytes);
        }
    }
    return it->second;
}

void assetIcon(const std::string& name) {
    const Texture& t = asset(name);
    if (t.valid()) {
        const float h = ImGui::GetTextLineHeight();
        ImGui::Image(t.id, ImVec2(h, h));
        ImGui::SameLine();
    }
}

} // namespace ke::ui::textures
