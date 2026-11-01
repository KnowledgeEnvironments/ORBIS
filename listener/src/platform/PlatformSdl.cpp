// The platform interfaces over SDL3, which already is the portable layer for everything here.
// A platform that needs more than SDL gives adds its own file beside this one.
#include "platform/Platform.hpp"
#include "chat/Wav.hpp"
#include "util/Product.hpp"


#include "drive/Commands.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>

#if !LISTENER_PHONE && (defined(SDL_PLATFORM_MACOS) || defined(SDL_PLATFORM_LINUX))
#define KE_LOOPBACK 1
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ke::platform {

bool openUrl(const std::string& url) {
    if (SDL_getenv("ORBIS_TEST") != nullptr) {
        return true; // a test must not fill a person's screen; it reads the address from the state
    }
    return SDL_OpenURL(url.c_str());
}

#ifdef KE_LOOPBACK
namespace {
int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string decoded(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2])));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

int bound(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // only this machine's browser can reach it
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}
} // namespace

LoopbackCallback::~LoopbackCallback() { stop(); }

bool LoopbackCallback::start(int preferredPort) {
    stop();
    // OpenAI's flow accepts any port, and only the port may vary
    listener_ = bound(preferredPort);
    if (listener_ < 0) {
        listener_ = bound(0);
    }
    if (listener_ < 0) {
        return false;
    }
    sockaddr_in a{};
    socklen_t n = sizeof a;
    getsockname(listener_, reinterpret_cast<sockaddr*>(&a), &n);
    port_ = ntohs(a.sin_port);
    return true;
}

bool LoopbackCallback::poll(std::map<std::string, std::string>& query) {
    if (listener_ < 0) {
        return false;
    }
    const int c = accept(listener_, nullptr, nullptr);
    if (c < 0) {
        return false;
    }
    timeval wait{1, 0}; // a browser sends at once; nothing here may hold a frame long
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
    char buf[8192];
    const ssize_t got = recv(c, buf, sizeof buf - 1, 0);
    std::string request(buf, got > 0 ? static_cast<std::size_t>(got) : 0);
    const std::string line = request.substr(0, request.find("\r\n"));
    const std::string head = "GET /auth/callback?";
    const bool ours = line.rfind(head, 0) == 0;
    const std::string body = ours ? "<!doctype html><title>ORBIS</title><p>Signed in. You can close this tab and go back to ORBIS.</p>"
                                  : "not here";
    const std::string reply = std::string("HTTP/1.1 ") + (ours ? "200 OK" : "404 Not Found") +
                              "\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\n\r\n" + body;
    send(c, reply.data(), reply.size(), 0);
    close(c);
    if (!ours) {
        return false; // a favicon, a stray visitor: keep listening
    }
    std::string q = line.substr(head.size());
    q = q.substr(0, q.find(' '));
    std::size_t at = 0;
    while (at <= q.size()) {
        const std::size_t end = std::min(q.find('&', at), q.size());
        const std::string pair = q.substr(at, end - at);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            query[pair.substr(0, eq)] = decoded(pair.substr(eq + 1));
        }
        at = end + 1;
    }
    return true;
}

void LoopbackCallback::stop() {
    if (listener_ >= 0) {
        close(listener_);
    }
    listener_ = -1;
    port_ = 0;
}
#else
// A phone has no browser that can come back to a loopback port: sign in from a desktop client.
LoopbackCallback::~LoopbackCallback() = default;
bool LoopbackCallback::start(int) { return false; }
bool LoopbackCallback::poll(std::map<std::string, std::string>&) { return false; }
void LoopbackCallback::stop() {}
#endif

void setClipboard(const std::string& text) { SDL_SetClipboardText(text.c_str()); }

std::string clipboard() {
    char* t = SDL_GetClipboardText();
    std::string out = t != nullptr ? t : "";
    SDL_free(t);
    return out;
}

bool notifySystem(const std::string& title, const std::string& body) {
#if defined(SDL_PLATFORM_MACOS) || defined(SDL_PLATFORM_LINUX)
    // Finished helpers, reaped on the next call: each lives a moment and nothing waits for it.
    static std::vector<SDL_Process*> running;
    std::erase_if(running, [](SDL_Process* p) {
        if (SDL_WaitProcess(p, false, nullptr)) {
            SDL_DestroyProcess(p);
            return true;
        }
        return false;
    });
#if defined(SDL_PLATFORM_MACOS)
    const char* args[] = {"/usr/bin/osascript",
                          "-e", "on run argv",
                          "-e", "display notification (item 2 of argv) with title (item 1 of argv)",
                          "-e", "end run",
                          "--", title.c_str(), body.c_str(), nullptr};
#else
    const char* args[] = {"notify-send", "--", title.c_str(), body.c_str(), nullptr};
#endif
    SDL_Process* p = SDL_CreateProcess(args, false);
    if (p == nullptr) {
        return false;
    }
    running.push_back(p);
    return true;
#else
    (void)title;
    (void)body;
    return false;
#endif
}

#if !defined(SDL_PLATFORM_MACOS)
void setBadge(int) {}
#endif

bool windowFocused(SDL_Window* window) {
    return window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

bool touchScreen() {
#if LISTENER_PHONE
    return true;
#else
    return false;
#endif
}

void showKeyboard(bool on, float x, float y, float w, float h) {
    SDL_Window* window = SDL_GetKeyboardFocus();
    if (window == nullptr) {
        return;
    }
    if (on) {
        const SDL_Rect r{static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h)};
        SDL_SetTextInputArea(window, &r, 0);
        SDL_StartTextInput(window);
    } else if (touchScreen()) {
        SDL_StopTextInput(window); // a desktop keeps text input on for the whole window
    }
}

void haptic(Haptic) {
    // SDL has no impact generator; iOS and Android add theirs here.
}

Insets safeArea(SDL_Window* window) {
    Insets out;
    SDL_Rect safe;
    int w = 0, h = 0;
    if (window == nullptr || !SDL_GetWindowSafeArea(window, &safe) || !SDL_GetWindowSize(window, &w, &h)) {
        return out;
    }
    out.top = static_cast<float>(safe.y);
    out.left = static_cast<float>(safe.x);
    out.bottom = static_cast<float>(h - safe.y - safe.h);
    out.right = static_cast<float>(w - safe.x - safe.w);
    return out;
}

bool canPickFolder() {
#if LISTENER_PHONE
    return false;
#else
    return true;
#endif
}

void pickFolder(SDL_Window* window, std::function<void(const std::string&)> done) {
    auto* held = new std::function<void(const std::string&)>(std::move(done));
    SDL_ShowOpenFolderDialog(
        [](void* user, const char* const* files, int) {
            std::unique_ptr<std::function<void(const std::string&)>> f(
                static_cast<std::function<void(const std::string&)>*>(user));
            (*f)(files != nullptr && files[0] != nullptr ? files[0] : "");
        },
        held, window, nullptr, false);
}

namespace {

constexpr int kVoiceRate = 16000;
SDL_AudioStream* g_recording = nullptr;
SDL_AudioStream* g_playing = nullptr;
bool g_fakeRecording = false;

bool audioUp() { return SDL_WasInit(SDL_INIT_AUDIO) != 0 || SDL_InitSubSystem(SDL_INIT_AUDIO); }

} // namespace

// A drive test has no microphone: this variable names a WAV to hand back, and no device is opened.
const char* fakeMic() { return SDL_getenv("ORBIS_FAKE_MIC"); }

bool startRecording() {
    if (fakeMic() != nullptr) {
        g_fakeRecording = true;
        return true;
    }
    if (g_recording != nullptr || !audioUp()) {
        return false;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, kVoiceRate};
    g_recording = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
    if (g_recording == nullptr) {
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(g_recording)) {
        SDL_DestroyAudioStream(g_recording);
        g_recording = nullptr;
        return false;
    }
    return true;
}

bool recording() { return g_recording != nullptr || g_fakeRecording; }

std::string stopRecording() {
    if (g_fakeRecording) {
        g_fakeRecording = false;
        std::size_t n = 0;
        void* data = SDL_LoadFile(fakeMic(), &n);
        std::string wav(data != nullptr ? static_cast<const char*>(data) : "", data != nullptr ? n : 0);
        SDL_free(data);
        return wav;
    }
    if (g_recording == nullptr) {
        return {};
    }
    const int bytes = SDL_GetAudioStreamAvailable(g_recording);
    std::string pcm(static_cast<std::size_t>(std::max(0, bytes)), '\0');
    const int got = bytes > 0 ? SDL_GetAudioStreamData(g_recording, pcm.data(), bytes) : 0;
    SDL_DestroyAudioStream(g_recording); // the device closes with it
    g_recording = nullptr;
    pcm.resize(static_cast<std::size_t>(std::max(0, got)));
    return pcm.empty() ? std::string() : chat::wavFromPcm16(pcm, kVoiceRate);
}

bool playWav(const std::string& wav) {
    if (!audioUp()) {
        return false;
    }
    SDL_IOStream* io = SDL_IOFromConstMem(wav.data(), wav.size());
    SDL_AudioSpec spec{};
    Uint8* samples = nullptr;
    Uint32 length = 0;
    if (io == nullptr || !SDL_LoadWAV_IO(io, true, &spec, &samples, &length)) {
        return false;
    }
    if (g_playing != nullptr) {
        SDL_DestroyAudioStream(g_playing);
    }
    g_playing = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    const bool ok = g_playing != nullptr && SDL_PutAudioStreamData(g_playing, samples, static_cast<int>(length)) &&
                    SDL_ResumeAudioStreamDevice(g_playing);
    SDL_free(samples);
    return ok;
}

std::vector<unsigned char> clipboardImage() {
    std::vector<unsigned char> out;
    if (!SDL_HasClipboardData("image/png")) {
        return out;
    }
    std::size_t n = 0;
    if (void* data = SDL_GetClipboardData("image/png", &n)) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        out.assign(bytes, bytes + n);
        SDL_free(data);
    }
    return out;
}

std::vector<SystemFont> systemFonts() {
    std::vector<SystemFont> out;
#if defined(SDL_PLATFORM_MACOS)
    const struct {
        const char* name;
        const char* path;
        bool mono;
    } kKnown[] = {
        {"Menlo", "/System/Library/Fonts/Menlo.ttc", true},
        {"Monaco", "/System/Library/Fonts/Monaco.ttf", true},
        {"Courier New", "/System/Library/Fonts/Supplemental/Courier New.ttf", true},
        {"Helvetica", "/System/Library/Fonts/Helvetica.ttc", false},
        {"Avenir Next", "/System/Library/Fonts/Avenir Next.ttc", false},
        {"Arial", "/System/Library/Fonts/Supplemental/Arial.ttf", false},
        {"Georgia", "/System/Library/Fonts/Supplemental/Georgia.ttf", false},
    };
    std::error_code ec;
    for (const auto& f : kKnown) {
        if (std::filesystem::exists(f.path, ec)) {
            out.push_back(SystemFont{f.name, f.path, f.mono});
        }
    }
#endif
    // Fonts a person installed: their own to use, and never shipped with the client. A name is the file's.
    std::vector<std::filesystem::path> dirs;
    if (char* pref = SDL_GetPrefPath(ke::kProductName, "listener")) {
        dirs.emplace_back(std::filesystem::path(pref) / "fonts");
        SDL_free(pref);
    }
#if defined(SDL_PLATFORM_MACOS)
    if (const char* home = SDL_getenv("HOME")) {
        dirs.emplace_back(std::filesystem::path(home) / "Library/Fonts");
    }
    dirs.emplace_back("/Library/Fonts");
#endif
    std::vector<SystemFont> own;
    for (const std::filesystem::path& dir : dirs) {
        std::error_code scan;
        for (const auto& e : std::filesystem::directory_iterator(dir, scan)) {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") {
                own.push_back(SystemFont{e.path().stem().string(), e.path().string(), false});
            }
        }
    }
    std::sort(own.begin(), own.end(), [](const SystemFont& a, const SystemFont& b) { return a.name < b.name; });
    out.insert(out.end(), own.begin(), own.end());
    return out;
}

bool fileDrop() {
#if LISTENER_PHONE
    return false;
#else
    return true;
#endif
}

} // namespace ke::platform
