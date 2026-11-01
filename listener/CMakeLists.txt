cmake_minimum_required(VERSION 3.28)
project(listener LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC OBJCXX) # before any dependency: a subdirectory cannot see a language enabled later
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(Warnings)
include(Dependencies)
include(Fonts)

find_package(Threads REQUIRED)

# --- listener_core: protocol, session, model, chat, workspace stubs. No SDL, no ImGui. ------------
# The shipped themes, embedded so a phone needs no bundle resource; prometheus first, the default.
file(GLOB LISTENER_THEME_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/*.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LISTENER_THEME_FILES})
set(LISTENER_THEME_INC "")
foreach(theme prometheus)
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/${theme}.json theme_text)
    string(APPEND LISTENER_THEME_INC "R\"THEME(${theme_text})THEME\",\n")
endforeach()
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_themes.inc CONTENT "${LISTENER_THEME_INC}" @ONLY)

add_library(listener_core STATIC
    src/net/Protocol.cpp
    src/net/WsClient.cpp
    src/net/Session.cpp
    src/model/EventStore.cpp
    src/chat/InputLine.cpp
    src/chat/MessageCache.cpp
    src/chat/Chat.cpp
    src/workspace/LocalFs.cpp
    src/workspace/Shell.cpp
    src/model/Principals.cpp
    src/model/FieldNews.cpp
    src/model/Activity.cpp
    src/model/Modeline.cpp
    src/chat/Transcript.cpp
    src/chat/KernelRecord.cpp
    src/model/Evaluations.cpp
    src/model/Graph.cpp
    src/model/WorldMap.cpp
    src/chat/Notifications.cpp
    src/model/Snapshot.cpp
    src/model/Tasks.cpp
    src/model/Tokens.cpp
    src/model/Actions.cpp
    src/chat/Views.cpp
    src/chat/Search.cpp
    src/chat/Symbols.cpp
    src/chat/FieldValue.cpp
    src/chat/Queue.cpp
    src/chat/Approvals.cpp
    src/chat/Conversations.cpp
    src/chat/Unsafe.cpp
    src/chat/RichText.cpp
    src/chat/PromptSections.cpp
    src/model/ObjectTree.cpp
    src/model/PaneFold.cpp
    src/model/PaneLayout.cpp
    src/modules/TerminalScreen.cpp
    src/modules/TextBuffer.cpp
    src/modules/FileListing.cpp
    src/modules/MathLayout.cpp
    src/modules/TexDocument.cpp
    src/model/ScratchFold.cpp
    src/chat/Attach.cpp
    src/app/Worlds.cpp
    src/net/OneShot.cpp
    src/util/Qr.cpp
    src/ui/ActionSchema.cpp
    src/ui/ThemeSpec.cpp
    src/ui/Themes.cpp)
target_include_directories(listener_core PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(listener_core PUBLIC
    vterm nlohmann_json::nlohmann_json readerwriterqueue ixwebsocket::ixwebsocket Threads::Threads
    $<$<BOOL:${ANDROID}>:log>) # util/Log.hpp mirrors to logcat there
listener_warnings(listener_core)

# --- listener: the windowed client ----------------------------------------------------------
# Android has no executables: the app is libmain.so, loaded by SDLActivity.
if(ANDROID)
    add_library(listener SHARED)
    set_target_properties(listener PROPERTIES OUTPUT_NAME main)
else()
    add_executable(listener)
endif()
target_sources(listener PRIVATE
    src/main.cpp
    src/app/App.cpp
    src/app/DebugHooks.cpp
    src/app/Layout.cpp
    src/app/Menus.cpp
    src/app/Lifecycle.cpp
    src/app/Prefs.cpp
    src/app/Runtime.cpp
    src/app/Smoke.cpp
    src/ui/Theme.cpp
    src/ui/Fonts.cpp
    src/ui/HistoryPane.cpp
    src/ui/SessionPane.cpp
    src/ui/InputPane.cpp
    src/ui/PrincipalsPane.cpp
    src/ui/ApprovalsPane.cpp
    src/ui/Chrome.cpp
    src/ui/RuleForm.cpp
    src/ui/ScratchPane.cpp
    src/ui/ReasoningRow.cpp
    src/ui/ChatGptSignIn.cpp
    src/ui/TerminalPane.cpp
    src/ui/terminal/Cards.cpp
    src/ui/terminal/Conversations.cpp
    src/ui/terminal/Devices.cpp
    src/ui/terminal/Fields.cpp
    src/ui/terminal/Markdown.cpp
    src/ui/terminal/Memory.cpp
    src/ui/terminal/Policy.cpp
    src/ui/terminal/Prompt.cpp
    src/ui/terminal/SelfPage.cpp
    src/ui/terminal/StatusBar.cpp
    src/ui/terminal/ModelPicker.cpp
    src/ui/terminal/Threads.cpp
    src/ui/terminal/Transcript.cpp
    src/ui/terminal/TranscriptView.cpp
    src/ui/GraphPane.cpp
    src/ui/NotificationsPane.cpp
    src/ui/ManualPane.cpp
    src/ui/OpenFileDialog.cpp
    src/ui/MessageImages.cpp
    src/ui/SelectableText.cpp
    src/ui/Markdown.cpp
    src/ui/SymbolUi.cpp
    src/ui/DeleteDialog.cpp
    src/ui/ActionForm.cpp
    src/ui/ActionDialog.cpp
    src/ui/Textures.cpp
    src/ui/ViewBlocks.cpp
    src/ui/ActivityMark.cpp
    src/ui/TabBar.cpp
    src/ui/TextMenu.cpp
    src/ui/UnsafeView.cpp
    src/ui/MathView.cpp
    src/modules/Multiplexer.cpp
    src/modules/TerminalModule.cpp
    src/modules/EditorModule.cpp
    src/modules/FilesModule.cpp
    src/modules/DocumentModule.cpp
    src/modules/ImageModule.cpp
    src/modules/Registry.cpp
    src/platform/PlatformSdl.cpp
    src/drive/Drive.cpp)
target_include_directories(listener PRIVATE third_party/stb) # stb_image.h (image views), pinned in third_party/README.md
target_link_libraries(listener PRIVATE listener_core imgui SDL3::SDL3)
listener_warnings(listener)

if(IOS)
    # One app bundle; fonts and (for development builds) the root token ride along as resources
    # and are read through SDL_GetBasePath(). Prefs and session.json live in SDL_GetPrefPath().
    set(LISTENER_BUNDLE_ID "dev.orbis.listener" CACHE STRING "iOS bundle identifier")
    option(LISTENER_BAKE_TOKEN "Copy LISTENER_TOKEN_FILE into the iOS bundle (development builds only)" ON)
    # The token the phone attaches with: a person's, never root's by default. Always named
    # root.token inside the bundle, which is a file name and not an identity.
    set(LISTENER_TOKEN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../data/person.token" CACHE FILEPATH
        "Token baked into the iOS bundle")
    file(GLOB LISTENER_FONT_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*.ttf)
    set(LISTENER_BUNDLE_RESOURCES ${LISTENER_FONT_FILES})
    set_source_files_properties(${LISTENER_FONT_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/fonts)
    list(APPEND LISTENER_BUNDLE_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/assets/server-96.png)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/assets/server-96.png PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set(LISTENER_TOKEN_SRC ${LISTENER_TOKEN_FILE})
    if(LISTENER_BAKE_TOKEN AND EXISTS ${LISTENER_TOKEN_SRC})
        configure_file(${LISTENER_TOKEN_SRC} ${CMAKE_CURRENT_BINARY_DIR}/bundle/root.token COPYONLY)
        list(APPEND LISTENER_BUNDLE_RESOURCES ${CMAKE_CURRENT_BINARY_DIR}/bundle/root.token)
        set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/bundle/root.token
            PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        message(STATUS "iOS bundle: baking ${LISTENER_TOKEN_FILE} (development build)")
    else()
        message(STATUS "iOS bundle: no token baked (set it in the app's Settings tab)")
    endif()
    target_sources(listener PRIVATE ${LISTENER_BUNDLE_RESOURCES})
    target_sources(listener PRIVATE platform/ios/Assets.xcassets)
    set_source_files_properties(platform/ios/Assets.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(listener PROPERTIES XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon)
    enable_language(OBJCXX)
    target_sources(listener PRIVATE src/platform/KeyboardIos.mm)
    set_source_files_properties(src/platform/KeyboardIos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_target_properties(listener PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER ${LISTENER_BUNDLE_ID}
        MACOSX_BUNDLE_BUNDLE_NAME ORBIS # with src/util/Product.hpp and the Android app_name
        MACOSX_BUNDLE_BUNDLE_VERSION 1
        MACOSX_BUNDLE_SHORT_VERSION_STRING 0.1
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${LISTENER_BUNDLE_ID}
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_INFOPLIST_KEY_UIApplicationSupportsIndirectInputEvents YES
        XCODE_ATTRIBUTE_ENABLE_BITCODE NO
        XCODE_ATTRIBUTE_SKIP_INSTALL NO)
    # SDL_RunApp on iOS needs these; SDL's static target lists them but Xcode's linker wants them
    # on the app too when the app itself holds main().
    target_link_libraries(listener PRIVATE "-framework UIKit" "-framework Foundation")
elseif(ANDROID)
    # platform/android/app/build.gradle packs the fonts; the themes are embedded. No token, ever.
    target_sources(listener PRIVATE src/platform/KeyboardAndroid.cpp)
else()
    # Debug builds read assets from the source tree; release builds look next to the binary.
    target_compile_definitions(listener PRIVATE
        $<$<CONFIG:Debug>:LISTENER_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets">)
    add_custom_command(TARGET listener POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:listener>/assets
        COMMENT "Copying assets next to the listener binary")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    enable_language(OBJCXX)
    target_sources(listener PRIVATE src/platform/BadgeMac.mm)
    target_link_libraries(listener PRIVATE "-framework AppKit")
endif()

# Tests, the mock world, and the smoke test are desktop only; the phones build just the app.
if(IOS OR ANDROID)
    return()
endif()

# --- listener_mockworld: stand-in world for tests and the demo ------------------------------
add_executable(listener_mockworld tools/mockworld.cpp)
target_include_directories(listener_mockworld PRIVATE tools)
target_link_libraries(listener_mockworld PRIVATE listener_core)
listener_warnings(listener_mockworld)

# --- tests ----------------------------------------------------------------------------------
enable_testing()
add_executable(listener_tests tests/wav_test.cpp tests/mux_test.cpp tests/terminal_screen_test.cpp tests/protocol_test.cpp tests/session_test.cpp tests/chat_test.cpp
    tests/principals_test.cpp tests/graph_test.cpp tests/worldmap_test.cpp tests/fieldnews_test.cpp tests/modeline_test.cpp tests/panes_test.cpp tests/markdown_test.cpp
    tests/snapshot_test.cpp tests/tasks_test.cpp tests/actionschema_test.cpp tests/views_test.cpp
    tests/threads_test.cpp tests/symbols_test.cpp tests/queue_test.cpp tests/approvals_test.cpp tests/conversations_test.cpp tests/theme_test.cpp tests/tree_test.cpp tests/kernel_record_test.cpp tests/search_test.cpp tests/worlds_test.cpp tests/tokens_test.cpp tests/qr_test.cpp
    src/modules/Registry.cpp tests/modules_test.cpp
    src/ui/Markdown.cpp src/ui/SelectableText.cpp) # the renderer and the selectable document, header-only on Theme
target_link_libraries(listener_tests PRIVATE listener_core imgui Catch2::Catch2WithMain)
target_compile_definitions(listener_tests PRIVATE ORBIS_HEADLESS
    LISTENER_PROTOCOL_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../docs/protocol-examples")
listener_warnings(listener_tests)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(listener_tests)

# End-to-end smoke: mockworld + listener --smoke on a private port, no window.
add_test(NAME listener_smoke
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.sh
            $<TARGET_FILE:listener_mockworld> $<TARGET_FILE:listener> 7781)
set_tests_properties(listener_smoke PROPERTIES TIMEOUT 60 LABELS e2e)

# Id sweep: a window, a synthetic mouse over every view; fails on any ImGui id conflict or error.
add_test(NAME listener_idsweep
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/idsweep.sh
            $<TARGET_FILE:listener_mockworld> $<TARGET_FILE:listener> 7786 16)
set_tests_properties(listener_idsweep PROPERTIES TIMEOUT 400 LABELS "e2e;window")

# UX through the drive API, always on a throwaway world (tests/ux/privateworld.py): its own port,
# its own data, a scripted model. Nothing here can reach the main world on 7777.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME listener_ux
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7820 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/smoke_drive.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {root}
                --out ${CMAKE_CURRENT_BINARY_DIR}/ux)
    set_tests_properties(listener_ux PROPERTIES TIMEOUT 420 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    # Panes use colour roles only: a literal colour outside the theme code fails.
    add_test(NAME listener_panes_ux
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/panes_drive.py
                --listener $<TARGET_FILE:listener> --mock $<TARGET_FILE:listener_mockworld> --port 7931)
    set_tests_properties(listener_panes_ux PROPERTIES TIMEOUT 180 LABELS ux)
    # One tree of views and panes, saved on the person.
    add_test(NAME listener_layout_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7846 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/layout_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/ux/layout)
    set_tests_properties(listener_layout_world PROPERTIES TIMEOUT 240 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    # The welcome and the notices, from a world as a new person finds it.
    add_test(NAME listener_map_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7836 --no-seed --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/map_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token} --root-token {root}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/ux/map)
    set_tests_properties(listener_map_world PROPERTIES TIMEOUT 300 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    # One door per agent, a person's own page, inspectors, pane names.
    add_test(NAME listener_marks_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7836 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/marks_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/ux/marks)
    set_tests_properties(listener_marks_world PROPERTIES TIMEOUT 300 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    add_test(NAME listener_chat_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7838 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/chat_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/ux/chat)
    set_tests_properties(listener_chat_world PROPERTIES TIMEOUT 300 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    add_test(NAME listener_idle_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7961 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/idle_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/ux/idle)
    set_tests_properties(listener_idle_world PROPERTIES TIMEOUT 120 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    add_test(NAME listener_large_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7962 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/large_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token} --root {root}
                --events 20000 --limit-s 10 --shots ${CMAKE_CURRENT_BINARY_DIR}/ux/large)
    set_tests_properties(listener_large_world PROPERTIES TIMEOUT 240 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    add_test(NAME listener_jump_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7941 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/jump_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/ux/jump)
    set_tests_properties(listener_jump_world PROPERTIES TIMEOUT 300 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    add_test(NAME listener_polish_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7836 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/polish_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/ux/polish)
    set_tests_properties(listener_polish_world PROPERTIES TIMEOUT 300 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    # Hard TeX in a chat, typeset by the world in the box, and a line that keeps taking typing, even
    # with the world gone.
    add_test(NAME listener_tex_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7837 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/tex_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/ux/tex)
    set_tests_properties(listener_tex_world PROPERTIES TIMEOUT 300 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    # A person's terminal shared with one agent, the red indicator, and OFF.
    add_test(NAME listener_share_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7825 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/share_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/ux/share)
    set_tests_properties(listener_share_world PROPERTIES TIMEOUT 300 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    # Hold TALK, read the words in the line, send them by hand; SPEAK on request; through a fake endpoint.
    add_test(NAME listener_voice_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7839 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/voice_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token} --root-token {root}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/ux/voice)
    set_tests_properties(listener_voice_world PROPERTIES TIMEOUT 300 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    # Images on Messages: dropped, picked, seen by a Model that takes them, a line for one that does not.
    add_test(NAME listener_image_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7842 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/image_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token} --root-token {root}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/ux/image)
    set_tests_properties(listener_image_world PROPERTIES TIMEOUT 300 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    add_test(NAME listener_no_literal_colours COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/no_literal_colours.py)
    # Every world-changing bracket, asserted on the log.
    add_test(NAME listener_world_ops
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7822 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/world_ops.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token} --token-ops
                --out ${CMAKE_CURRENT_BINARY_DIR}/ops)
    set_tests_properties(listener_world_ops PROPERTIES TIMEOUT 420 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    # A real PTY and a real file behind the pane modules.
    add_test(NAME listener_panes_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7824 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/panes_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/panes-world)
    set_tests_properties(listener_panes_world PROPERTIES TIMEOUT 420 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    # The same terminals in a real holder process: skipped until `make holder` has built one.
    add_test(NAME listener_panes_holder
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7826 --holder --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/panes_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/panes-holder)
    set_tests_properties(listener_panes_holder PROPERTIES TIMEOUT 420 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    # A long Field in the editor, from the row to the next prompt, on a real wake.
    add_test(NAME listener_field_editor
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7838 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/field_editor.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token} --root-token {root}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/ux/field-editor)
    set_tests_properties(listener_field_editor PROPERTIES TIMEOUT 300 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    # The manual, edits a Principal hears about, compression, leaving the box.
    add_test(NAME listener_manual_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7833 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/manual_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token} --root-token {root}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/ux/manual)
    set_tests_properties(listener_manual_world PROPERTIES TIMEOUT 300 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
    # The Model control and Sign in with ChatGPT, against a stub of OpenAI's documented endpoints.
    add_test(NAME listener_model_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7839 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/model_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token} --stub-port 7897
                --shots ${CMAKE_CURRENT_BINARY_DIR}/ux/model)
    set_tests_properties(listener_model_world PROPERTIES TIMEOUT 300 LABELS "ux;window;private-world"
        ENVIRONMENT "KE_OPENAI_AUTH=http://127.0.0.1:7897;KE_OPENAI_API=http://127.0.0.1:7897/v1")
    # Unsafe mode, APPROVE ALL, every menu item, the tree and nicknames, on real wakes.
    add_test(NAME listener_ui_world
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/privateworld.py --port 7826 --
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ux/ui_world.py
                --listener $<TARGET_FILE:listener> --url {url} --token-file {token}
                --shots ${CMAKE_CURRENT_BINARY_DIR}/ui-world)
    set_tests_properties(listener_ui_world PROPERTIES TIMEOUT 420 LABELS "ux;window;private-world" SKIP_RETURN_CODE 77)
endif()
