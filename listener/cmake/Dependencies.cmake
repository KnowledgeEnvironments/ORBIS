# Every third-party pin lives here (ADR 0007). FETCHCONTENT_BASE_DIR is set by the presets to
# listener/third_party/_deps (macOS) or listener/third_party/_deps/<preset> (iOS), all git-ignored.
# On macOS the system libraries (SDL3, FreeType) come from Homebrew via CMAKE_PREFIX_PATH;
# /opt/local is ignored so MacPorts SDL2/FreeType never leak in. On iOS there is no Homebrew, so
# SDL3 and FreeType are built from source as static libraries and exported under the same target
# names (SDL3::SDL3, Freetype::Freetype); nothing below this file knows the difference.
# LISTENER_DEPS_FROM_SOURCE=ON takes the same from-source path on macOS (preset macos-source), for a
# machine without Homebrew such as vantage.
option(LISTENER_DEPS_FROM_SOURCE "Build SDL3 and FreeType from source instead of find_package" OFF)
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
# Inside a confined shell there is no network: dependencies already on disk are used as they are.
if(DEFINED ENV{ORBIS_BOX})
    set(FETCHCONTENT_FULLY_DISCONNECTED ON)
endif()

# --- header/source-only libraries ------------------------------------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b-docking
    GIT_SHALLOW    TRUE)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")

FetchContent_Declare(readerwriterqueue
    GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
    GIT_TAG        v1.0.7
    GIT_SHALLOW    TRUE)

# --- IXWebSocket: plain ws:// only for sprint 0 ----------------------------------------------
FetchContent_Declare(ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        v12.0.1
    GIT_SHALLOW    TRUE)
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)

# --- Catch2 (tests; not built for iOS) --------------------------------------------------------
FetchContent_Declare(catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE)
set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(nlohmann_json readerwriterqueue ixwebsocket)
if(NOT IOS AND NOT ANDROID)
    FetchContent_MakeAvailable(catch2)
endif()
FetchContent_MakeAvailable(imgui)  # imgui has no CMakeLists; the target is built below

# libvterm: the terminal state machine behind the terminal module. Plain C, tables committed in the
# repository, so it builds the same on macOS and iOS with no perl step. No CMakeLists of its own.
FetchContent_Declare(libvterm
    GIT_REPOSITORY https://github.com/neovim/libvterm.git
    GIT_TAG        934bc2fbf21800ac3458a499df8820ca5fb45fd3)
FetchContent_MakeAvailable(libvterm)
add_library(vterm STATIC
    ${libvterm_SOURCE_DIR}/src/encoding.c
    ${libvterm_SOURCE_DIR}/src/keyboard.c
    ${libvterm_SOURCE_DIR}/src/mouse.c
    ${libvterm_SOURCE_DIR}/src/parser.c
    ${libvterm_SOURCE_DIR}/src/pen.c
    ${libvterm_SOURCE_DIR}/src/screen.c
    ${libvterm_SOURCE_DIR}/src/state.c
    ${libvterm_SOURCE_DIR}/src/unicode.c
    ${libvterm_SOURCE_DIR}/src/vterm.c)
target_include_directories(vterm PUBLIC ${libvterm_SOURCE_DIR}/include)
set_target_properties(vterm PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)

# --- SDL3 and FreeType -----------------------------------------------------------------------
if(IOS OR ANDROID OR LISTENER_DEPS_FROM_SOURCE)
    # Same versions as the Homebrew bottles used on macOS (sdl3 3.4.18, freetype 2.14/2.15 line).
    FetchContent_Declare(sdl3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-3.4.18
        GIT_SHALLOW    TRUE
        PATCH_COMMAND  ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/PatchSdl.cmake)
    # Android loads libSDL3.so and libmain.so from SDLActivity, SDL's tested shape; static elsewhere.
    if(ANDROID)
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
        set(SDL_ANDROID_JAR OFF CACHE BOOL "" FORCE) # platform/android holds SDL's Java sources
    else()
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
    endif()
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_UNINSTALL OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(freetype
        GIT_REPOSITORY https://github.com/freetype/freetype.git
        GIT_TAG        VER-2-14-3
        GIT_SHALLOW    TRUE)
    # Standalone: ImGui only needs the rasterizer, and nothing else on iOS (or a bare Mac) provides these.
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    set(FT_ENABLE_ERROR_STRINGS OFF CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(sdl3 freetype)

    # SDL's own CMake defines SDL3::SDL3 as an alias of the static library when SDL_SHARED is
    # off; FreeType defines Freetype::Freetype. Both are checked here so a version bump that
    # renames them fails at configure time, not in the link.
    if(NOT TARGET SDL3::SDL3)
        add_library(SDL3::SDL3 ALIAS SDL3-static)
    endif()
    if(NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype ALIAS freetype)
    endif()
else()
    find_package(SDL3 CONFIG REQUIRED)
    find_package(Freetype REQUIRED)
endif()

# --- Dear ImGui (docking) + SDL3 / SDL_GPU backends + FreeType rasterizer (ADR 0005) ---------
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp  # iOS simulator only (no SDL_GPU there)
    ${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ImguiHooks.cpp)
target_include_directories(imgui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
    ${imgui_SOURCE_DIR}/misc/freetype)
target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_FREETYPE IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_ENABLE_TEST_ENGINE)
target_link_libraries(imgui PUBLIC SDL3::SDL3 Freetype::Freetype)
target_compile_features(imgui PUBLIC cxx_std_20)
