// What differs between a desktop, a phone and a tablet, behind one header. Panes and modules call
// these and never SDL or the OS; each platform implements them in one file. The client assumes no
// local shell, PTY or filesystem anywhere else: terminals, files, TeX and search live in the world.
#pragma once

#include <SDL3/SDL_platform_defines.h>

// LISTENER_PHONE selects the phone frame, the session lifecycle and the Settings-driven start;
// everything else about a phone is the same code on iOS and Android.
#if defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_ANDROID)
#define LISTENER_PHONE 1
#else
#define LISTENER_PHONE 0
#endif

#include <functional>
#include <map>
#include <string>
#include <vector>

struct SDL_Window;

namespace ke::platform {

// The OS clipboard, both ways.
void setClipboard(const std::string& text);
std::string clipboard();
// A PNG on the clipboard, or nothing.
std::vector<unsigned char> clipboardImage();

// A person's finger, not a mouse: long-press menus, bigger targets, the soft key row.
bool touchScreen();
// The on-screen keyboard, on platforms that have one. `over` is the rect it should keep visible.
void showKeyboard(bool on, float x = 0.0f, float y = 0.0f, float w = 0.0f, float h = 0.0f);
enum class Haptic { Tap, Warning };
void haptic(Haptic kind);

struct Insets {
    float top = 0.0f, left = 0.0f, bottom = 0.0f, right = 0.0f;
};
Insets safeArea(SDL_Window* window);
// The OS's own insets for the window, when it has them: on iOS SDL's answer can be the other
// orientation's (iPhone Duo, 2026-11-11: an 84 pt side band in portrait). False means use SDL's.
bool nativeSafeArea(SDL_Window* window, Insets& out);

// A folder chosen by the person, on platforms that can; `done` gets "" when cancelled.
bool canPickFolder();
void pickFolder(SDL_Window* window, std::function<void(const std::string&)> done);

// The font files this platform can offer by name, on top of the bundled ones.
struct SystemFont {
    std::string name;
    std::string path;
    bool mono = false;
};
std::vector<SystemFont> systemFonts();

// A line in the OS's own notification centre. False where a platform has none, or the call failed;
// the text goes as an argument, never through a shell. Phones need a push relay: see the punch list.
bool notifySystem(const std::string& title, const std::string& body);
// A count on the app's icon, where the platform has one; 0 clears it.
void setBadge(int count);
// Whether this window has the keyboard: a person looking at it needs no OS notification.
bool windowFocused(SDL_Window* window);

// Voice. The microphone opens at startRecording and closes at stopRecording: nothing listens between.
// One recording at a time; stopRecording hands back a 16 kHz mono WAV, "" when there was nothing.
bool startRecording();
std::string stopRecording();
bool recording();
// Plays a WAV to the end, replacing what was playing. False when the device or the bytes refuse.
bool playWav(const std::string& wav);

// The person's browser. False where it could not be opened; a run under the drive API opens nothing.
bool openUrl(const std::string& url);

// A one-shot http listener on 127.0.0.1 for a browser's redirect, on platforms that can hold one.
// Polled from the frame, so nothing here needs a thread. Port 0 until started.
class LoopbackCallback {
public:
    ~LoopbackCallback();
    bool start(int preferredPort);
    int port() const { return port_; }
    // The query of a request to /auth/callback, decoded, once one has arrived.
    bool poll(std::map<std::string, std::string>& query);
    void stop();

private:
    int listener_ = -1;
    int port_ = 0;
};

// Files dropped on the window arrive through the app's event loop; true where that can happen.
bool fileDrop();

} // namespace ke::platform
