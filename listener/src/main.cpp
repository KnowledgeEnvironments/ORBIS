// ORBIS listener: attach, subscribe, show the rows and the chat lines, type on the input line;
// drop and resume without a gap.
//   listener [--url ws://127.0.0.1:7777/ws] [--format json] [--token <s> | --token-file <p>]
//            [--principal <uuid>] [--smoke [seconds]]
// On a phone there is no command line: prefs.json (SDL_GetPrefPath), then on iOS the token baked
// into the bundle by a development build (LISTENER_TOKEN_FILE, the person's by default), then the
// compiled defaults below. Android bakes nothing: the way in is PAIR WITH CODE. A client is a person; root is the agent.
#include "app/App.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include "app/Options.hpp"
#include "app/Runtime.hpp"
#include "app/Smoke.hpp"
#include "platform/Platform.hpp"
#include "util/Log.hpp"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <string>

#if LISTENER_PHONE
#include "app/Prefs.hpp"
#include "util/Product.hpp"

#include <SDL3/SDL_main.h> // SDL_RunApp on iOS, SDLActivity on Android: main becomes SDL_main
#include <filesystem>
#include <fstream>
#endif

namespace {

#if !LISTENER_PHONE

void usage() {
    std::fprintf(stderr,
                 "ORBIS listener\nusage: listener [--url ws://127.0.0.1:7777/ws] [--format json]\n"
                 "                [--token <string> | --token-file <path>]   (default ../data/person.token)\n"
                 "                [--principal <uuid>] [--smoke [seconds]]\n"
                 "                [--drive <unix-socket>]   (UX drive API, also LISTENER_DRIVE=<path>)\n"
                 "                [--data-dir <dir>]        (session.json and prefs.json; default Application Support)\n");
}

std::string assetsDir() {
#ifdef LISTENER_ASSETS_DIR
    return LISTENER_ASSETS_DIR;
#else
    const char* base = SDL_GetBasePath();
    return std::string(base ? base : "./") + "assets";
#endif
}

#else // LISTENER_PHONE

#ifndef ORBIS_DEFAULT_URL
#define ORBIS_DEFAULT_URL "ws://127.0.0.1:7777/ws" // a phone pairs, or set this at build time
#endif
constexpr const char* kPhoneDefaultUrl = ORBIS_DEFAULT_URL;

std::string trimmed(std::string t) {
    while (!t.empty() && (t.back() == '\r' || t.back() == '\n' || t.back() == ' ')) {
        t.pop_back();
    }
    return t;
}

std::string readBundledToken(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::string t;
    if (in) {
        std::getline(in, t);
    }
    return trimmed(t);
}

// Precedence: prefs.json (what the Settings tab saved) over compiled defaults; a token
// baked into the bundle replaces the saved token the first time this build sees it, so a
// reinstall against a world with a fresh token does not keep attaching with the stale one.
ke::Options phoneOptions() {
    ke::Options opts;
    const char* base = SDL_GetBasePath();
    const std::filesystem::path bundle = base ? base : "./";
    char* pref = SDL_GetPrefPath(ke::kProductName, "listener");
    const std::filesystem::path dataDir = pref ? pref : "./";
    SDL_free(pref);

    const std::filesystem::path prefsFile = dataDir / "prefs.json";
    ke::Prefs prefs = ke::loadPrefs(prefsFile).value_or(ke::Prefs{});
    const std::string bundled = readBundledToken(bundle / "root.token");
    if (prefs.url.empty()) {
        prefs.url = kPhoneDefaultUrl;
    }
    if (!bundled.empty() && bundled != prefs.bundledToken) {
        ke::log::info("token: adopting the token baked into the bundle");
        prefs.token = bundled;
        prefs.bundledToken = bundled;
    }
    if (prefs.token.empty()) {
        opts.tokenProblem = "no token: none saved and none baked into the bundle; set one in Settings";
        ke::log::warn("{}", opts.tokenProblem);
    }
    ke::savePrefs(prefsFile, prefs);

    opts.url = prefs.url;
    opts.token = prefs.token;
    opts.principal = prefs.principal;
    opts.assetsDir = bundle.string();
    while (opts.assetsDir.size() > 1 && opts.assetsDir.back() == '/') {
        opts.assetsDir.pop_back();
    }
    opts.dataDir = dataDir.string();
#if defined(SDL_PLATFORM_ANDROID)
    opts.assetsDir = "assets"; // inside the APK: relative, so SDL's IO opens it through AssetManager
#endif
#if defined(TARGET_OS_SIMULATOR) && TARGET_OS_SIMULATOR
    // On the simulator the drive API opens a socket in the host's home, so the phone build can be
    // walked and screenshotted from the Mac like the desktop (a unix socket path is short, so not
    // the app container's). A device build never has one.
    if (const char* home = std::getenv("SIMULATOR_HOST_HOME"); home != nullptr) {
        opts.driveSocket = std::string(home) + "/.orbis/sim-drive.sock";
    }
#endif
    ke::log::info("phone: prefs {} assets {}", prefsFile.string(), opts.assetsDir);
    return opts;
}

#endif

} // namespace

int main(int argc, char** argv) {
#if LISTENER_PHONE
    (void)argc;
    (void)argv;
    return ke::runApp(phoneOptions());
#else
    ke::Options opts;
    std::string token, tokenFile;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", flag);
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--url") {
            opts.url = value("--url");
        } else if (a == "--format") {
            opts.format = value("--format");
        } else if (a == "--token") {
            token = value("--token");
        } else if (a == "--token-file") {
            tokenFile = value("--token-file");
        } else if (a == "--principal") {
            opts.principal = value("--principal");
        } else if (a == "--smoke") {
            opts.smokeSeconds = 3;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                opts.smokeSeconds = std::atoi(argv[++i]);
            }
        } else if (a == "--drive") {
            opts.driveSocket = value("--drive");
        } else if (a == "--data-dir") {
            // session.json and prefs.json live here instead of Application Support: a second
            // listener on the same Mac (a UX test beside the one you use) must not share the
            // session id, or each attach closes the other's socket (the world keeps one channel
            // per session).
            opts.dataDir = value("--data-dir");
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    if (opts.format != "json") {
        ke::log::error("--format {} is not supported; the listener speaks the JSON form (ADR 0002)",
                       opts.format);
        return 2;
    }
    opts.token = ke::readToken(token, tokenFile, argv[0], opts.tokenProblem);
    opts.assetsDir = assetsDir();
    if (opts.driveSocket.empty()) {
        if (const char* d = std::getenv("LISTENER_DRIVE")) {
            opts.driveSocket = d;
        }
    }

    if (opts.smokeSeconds > 0) {
        if (opts.token.empty()) {
            return 2;
        }
        return ke::runSmoke(opts);
    }
    return ke::runApp(opts);
#endif
}
