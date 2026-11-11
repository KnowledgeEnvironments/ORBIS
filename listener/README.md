# ORBIS listener

The native desktop client of ORBIS (spec section 15), the one human surface. It attaches to the
world over `ws://host:7777/ws?format=json`, subscribes to one cursor over the log, shows the rows
and the chat lines, takes typed lines on an input line, and survives a dropped connection without
losing or duplicating an event.

C++20, SDL3 + SDL_GPU (Metal) + Dear ImGui with FreeType (ADR 0005), IXWebSocket,
nlohmann/json, moodycamel readerwriterqueue, Catch2, all vendored by FetchContent with pinned tags
(ADR 0007, `cmake/Dependencies.cmake`). No web UI, no Objective-C++. The same sources build the
iOS app (see "iOS" below).

## Build

Requirements: Xcode clang, CMake >= 3.28, Ninja, Homebrew `sdl3` and `freetype`. The presets set
`CMAKE_PREFIX_PATH=/opt/homebrew` and ignore `/opt/local` so MacPorts SDL2/FreeType never leak in.
The first configure downloads the dependencies into `third_party/_deps/` (git-ignored) and the
fonts into `assets/fonts/` (OFL; JetBrains Mono from a pinned google/fonts commit, fetched with
`/usr/bin/curl`; Inter is still fetched but no longer loaded). If the font download fails the
listener falls back to Menlo from `/System/Library/Fonts` at runtime.

```sh
cd listener
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug          # Catch2 unit tests + the end-to-end smoke test
```

`macos-release` is the same with `CMAKE_BUILD_TYPE=Release`; it reads assets next to the binary
(`SDL_GetBasePath()`), the debug build reads them from the source tree (`LISTENER_ASSETS_DIR`).
`make listener` from the repo root runs the debug configure + build; `make fmt` runs clang-format.

Targets: `imgui` (static), `listener_core` (static: net, model, chat, workspace; no SDL/ImGui, so
it is testable), `listener`, `listener_mockworld`, `listener_tests` (the last two are not built
for iOS).

## Run

```sh
./build/macos-debug/listener_mockworld &        # stand-in world on 7777 when the real one is down
./build/macos-debug/listener --token x
```

Flags:

| Flag | Default | Meaning |
| --- | --- | --- |
| `--url <ws url>` | `ws://127.0.0.1:7777/ws` | the world; `?format=json` is appended |
| `--format json` | `json` | fixed; the listener speaks the JSON form (ADR 0002) |
| `--token <s>` / `--token-file <p>` | `../data/person.token` (relative to the repo) | the principal's credential; a client is a person, root is the agent, so root's token is only ever passed explicitly and a missing token leaves the client unattached with the reason in SESSION |
| `--principal <uuid>` | none | sent in `attach` when given |
| `--smoke [seconds]` | off | run network + session + store + chat with no window, drop the connection halfway, send one line, ask `who am i`, print a summary, exit 0 iff no gap and both were answered |

Environment: `LISTENER_TRACE=1` logs every push. `LISTENER_SHOT=<file.bmp>` (debug aid) renders
one frame into an offscreen texture after `LISTENER_SHOT_AFTER_MS` (4000) ms, optionally after
typing `LISTENER_SHOT_TYPE` into the input line, writes it and exits; for checking the look on a
machine whose screen cannot be captured. All log lines carry the prefix `orbis`.

## The look

`docs/listener-style.md` is the rule: 90s high-contrast hacker, phosphor on black, angular,
whimsy rationed. In code (`src/ui/Theme.*`, `src/ui/Fonts.*`):

- Palette: background `#000000`; phosphor `#00FF41` for text, borders and the cursor; phosphor
  dim `#00A32B` for secondary text, grid lines and timestamps; amber `#FFB000` for field events
  and values; cyan `#00FFFF` for lifecycle events and object references (messages included);
  magenta `#FF00FF` for access; white `#FFFFFF` for the head seq, the boot lines and the prompt;
  red `#FF3B30` for errors, gated, stalled. No other colour appears; no gradients, shadows,
  transparency or rounding (`ImGuiStyle` rounding 0, borders 1 px, anti-aliasing off).
- Inverse video for selection, focus and the active tab: black on phosphor, never a tint.
- One face, JetBrains Mono 14 px, rasterised by FreeType with
  `ImGuiFreeTypeLoaderFlags_Monochrome | MonoHinting` (the `BuilderFlags` names in the style
  doc are the older spellings of the same bits), so glyphs are crisp and aliased.
- Pane titles in a 1 px chamfered box, uppercase and letter-spaced: `H I S T O R Y`,
  `S E S S I O N`, `B I N D`, `I N P U T`. Panes have a 1 px phosphor outline with 6 px
  45-degree cuts at the top-left and bottom-right corners, drawn with the draw list.
- Kind chips are bracketed uppercase tokens in the family colour: `[FIELD-SET]`, `[ATTACHED]`,
  `[GRANTED]`; chat lines carry `[MESSAGE]` in cyan and `who am i` rows `[WHO]` in amber.
  Buttons are bracketed words, `[ DROP ]`, `[ SEND BIND OPS ]`, inverse while pressed.
- Tab headers (the phone) are 15-degree parallelograms, inverse when active. Lag and the
  unacknowledged window are rows of slanted bars `////....`.
- Whimsy, each with an off switch in **Settings** (desktop: the SETTINGS menu; phone: the
  Settings tab; saved in `prefs.json`): a four-line boot sequence on attach over 300 ms
  (`ORBIS LISTENER v0 // CONTACT`, `RESOLVING <principal>`, `HEAD <seq>`, `READY.` followed by
  the blinking block cursor; off prints one line `ORBIS LISTENER // ATTACHED AS ... // HEAD ...`);
  the block cursor blinking with a 530 ms period; `CARRIER LOST` in red in the session pane while
  dropped and `CARRIER // RESUMED FROM <acked>` in phosphor after the reattach; a spinner
  `| / - \` at 120 ms on rows whose Evaluation streams (the hook exists, nothing streams yet); and
  a scanline overlay (every other line at 8 % black), off by default and never on the phone.
- Idle rule kept: the loop blocks in `SDL_WaitEventTimeout` until the next computed deadline (a
  blink edge, a spinner frame, a boot line, the 5 s route retry) or the socket's wake event, and
  only while the window is visible; one frame per deadline, no spinning.

## Panes

Default desktop layout, one tree of tabs and splits a person rearranges (`view/layout`): **HISTORY** left
70 %, **SESSION** over **BIND** right 30 %, **INPUT** as a strip along the bottom.

- **HISTORY**: `SEQ | AT | KIND | STREAM | ACTOR | BODY`, full ids and the pretty body on hover,
  follows the tail until you scroll up (`[ FOLLOW ]` toggles). A `sent` or `delivered` row whose
  Message has been read shows as a chat line, `<from-prefix> > text`, with `<- <prefix>` when it
  is a reply and `// sent` / `// delivered to <prefix>` dim at the end. The chat's local notes
  (boot lines, the `> text` echo of a line you sent, `[WHO]` rows, `[ SEND: ... ]` errors) are
  merged into the same list in order. Click a row for inverse-video selection.
- **SESSION**: CARRIER state, url, principal, session id, head seq in white, attaches, frames,
  rows and gaps, the inbox route, the last error in red; per subscription cursor, acked, and
  unacked/lag as slanted bars against the window.
- **BIND**: the sprint-0 workspace stubs and what the world answered (`OK SEQ n` or `ERR CODE`).
- **INPUT**: `>` in white, the line, the block cursor, `ROUTE <prefix>` or `NO ROUTE` at the right.

## The input line

`src/ui/InputPane.*` draws it; `src/chat/*` (in `listener_core`, tested without a UI) does the
work:

- Target: root's inbox Route. After attach the listener sends `read {object <principal>}` and
  takes the first id of `principal/routes` (top-level or folded into `object/fields`, see
  `CONTRACT-NEEDS.md` 5). When absent it shows `NO ROUTE` and reads again every 5 s.
- Enter sends `{op send, args {to [<principal>], route <route-id>, text <line>, tags []}}`.
  `err not-implemented` is shown once as `[ SEND: NOT BUILT ]`, `unresolved` as
  `[ SEND: UNRESOLVED ]`, a send with no route as `[ SEND: NO ROUTE ]`. The line itself echoes dim
  as `> text`; the real row arrives with the pushed `sent` event.
- Up and down arrows recall the last 50 lines (`chat::InputLine`).
- `who am i` sends a `read` of the principal and prints its fields as `[WHO]` rows (a real query,
  not a trick).
- A token `@<8+ hex chars>` becomes a ref tag when some object id seen in the store (event,
  stream, actor, or a uuid-looking body value) starts with it.
- Message rows: every `sent` / `delivered` event names a Message in its body; the listener reads
  it once (`chat::MessageCache`, keyed by id, shared by `sent` and `delivered`) and renders the
  text. Reads lost to a dropped connection are reissued after the reattach.

Session state (`{session, acked per sub}`) persists in
`~/Library/Application Support/ORBIS/session.json`, the whimsy switches in `prefs.json` beside
it, so a restarted listener reattaches with `after = acked`. Delete `session.json` to start from
the world's head again (useful after restarting the mock, whose log restarts at 42). Nothing is
migrated from the old `ORBIS/` directory.

### Verifying without a screen

`listener --smoke 4` against the mock prints a summary such as

```
attaches 2  resumed 1  socket opens 2
rows 23  (before drop 12, after 11)  head 68
gaps 0  duplicates(store) 0
route        found 5a1f7b0e-7d6b-4b1f-9a3e-2b2d9a5e0101
send         ok message 6d0e5a7e-2222-4c2a-8f1e-000000000001
messages     1 read, 0 pending; received: smoke: hello from ORBIS
who am i     7 row(s)
[ok] no gap in the event store
[ok] send was answered (ok or an err such as not-implemented)
[ok] the sent Message came back as sent/delivered rows and read
[ok] who am i printed the principal's fields
```

and `ctest` runs the same flow on port 7781 as `listener_smoke`. Against the real world, whose
root has no inbox Route yet, the route is `none`, the send is probed with the principal id in the
route's place and `err not-implemented` (or `unresolved`) counts as answered. The windowed binary
logs `screensaver check OK: SDL_ScreenSaverEnabled() is true` right after window creation and
exits with status 3 if the check fails.

## iOS

One codebase, two platforms: the iOS app is this listener built with `CMAKE_SYSTEM_NAME=iOS`.
SDL3 and FreeType are then built from source by FetchContent (pinned `release-3.4.18` and
`VER-2-14-3`, static; Homebrew's are macOS only; FreeType without zlib/bzip2/png/harfbuzz/brotli)
and exported under the same target names, so the rest of the CMake is shared. Tests, the mock
world and `--smoke` are not built for iOS; only the `listener` app bundle is.

Presets (Xcode generator, deployment target 17.0, arm64, dependencies in
`third_party/_deps/<preset>/`, products in `build/<preset>/Debug-<sdk>/listener.app`):

| Preset | SDK | Signing |
| --- | --- | --- |
| `ios-device` | iphoneos | automatic, team `YOURTEAMID`, identity "Apple Development", bundle id `dev.orbis.listener` (`-DLISTENER_BUNDLE_ID=...` to change) |
| `ios-simulator` | iphonesimulator | ad hoc (`-`) |

```sh
cd listener
cmake --preset ios-device              # the first configure clones SDL3 and FreeType (~3 min)
cmake --build --preset ios-device -- -allowProvisioningUpdates
xcrun devicectl device install app --device <udid> build/ios-device/Debug-iphoneos/listener.app
xcrun devicectl device process launch --console --device <udid> dev.orbis.listener
#   --console streams the app's log (stderr) to the terminal; omit it to leave the app running

cmake --preset ios-simulator && cmake --build --preset ios-simulator
xcrun simctl boot "iPhone 17"
xcrun simctl install booted build/ios-simulator/Debug-iphonesimulator/listener.app
xcrun simctl launch --console booted dev.orbis.listener
```

Signing works without an Apple ID in Xcode as long as the team's development profile is
installed (`~/Library/Developer/Xcode/UserData/Provisioning Profiles`, the Xcode-managed
"iOS Team Provisioning Profile: *") and lists the device; a new device needs Xcode with the
account signed in once. The team id is the certificate's OU (`YOURTEAMID`), not the id in its
name. Xcode-managed profiles cannot be used with manual signing, hence automatic.

What differs on iOS (all `#if SDL_PLATFORM_IOS` in `src/main.cpp` and `src/app/App.cpp`):

- No flags. Options come from `prefs.json`, then the `root.token` in the bundle, then the
  compiled defaults (`ws://192.0.2.10:7777/ws`, JSON form). The **SETTINGS** tab edits url,
  token and principal (`[ APPLY AND RECONNECT ]` saves and restarts the network stack), has
  `[ DROP ]`, `[ RECONNECT ]` and `[ FORGET SESSION ]`, and the whimsy switches (no scanlines).
- `root.token`: a development configure copies `LISTENER_TOKEN_FILE` (default `../data/person.token`) into the bundle under this file name when it
  exists (`-DLISTENER_BAKE_TOKEN=OFF` to skip). The app adopts a newly baked token once and
  remembers which one (`bundled-token` in prefs), so a token typed in Settings survives
  reinstalls of the same build but a rebuild against a world with a fresh token wins.
- Files: `SDL_GetPrefPath("ORBIS", "listener")`, i.e.
  `<sandbox>/Library/Application Support/ORBIS/listener/{prefs.json,session.json}`
  (`session.json` has the same `{session, acked}` content as on macOS). Fonts come from the
  bundle (`SDL_GetBasePath()/fonts`), the token from `SDL_GetBasePath()/root.token`.
- Window: fullscreen, high pixel density, UI scale 1.15 x, the work area inset to the safe area
  (`SDL_GetWindowSafeArea`). One window, no dockspace: a strip of parallelogram tabs
  HISTORY / SESSION / SETTINGS (SESSION includes BIND), the active pane's body, and the INPUT
  strip always at the bottom. Tapping the line starts text input through the ImGui backend
  (`SDL_StartTextInput`), and the app sets `SDL_SetTextInputArea` to the field's rectangle each
  frame while it is active so SDL keeps the strip above the keyboard. Touch: SDL's
  touch-to-mouse synthesis drives ImGui; a finger drag scrolls a pane or the history table
  (`ui::touchDragScroll`, a no-op for a mouse).
- Lifecycle: `SDL_EVENT_WILL_ENTER_BACKGROUND` closes the socket without `detach` (the world
  keeps the session; `WsClient::suspend()`), `SDL_EVENT_DID_ENTER_FOREGROUND` reconnects,
  which reattaches with the persisted session and acked seqs (`resumed=true`, no gap). The
  close happens in an `SDL_AddEventWatch` callback because the main loop may not run again
  before the process is suspended; the callback must not block (SDL holds its watcher lock
  while calling it and the socket thread needs that lock to post the wake event, so joining
  the socket thread there deadlocks and iOS kills the app with `0x8BADF00D`), so the join and
  the reconnect run from the main loop. Nothing is rendered while backgrounded, and a frame
  whose swapchain size no longer matches the layout is skipped.
- Simulator: SDL_GPU's Metal backend needs `MTLGPUFamilyApple3`, which the simulator does not
  report, so simulator builds fall back to ImGui's SDL_Renderer backend (SDL's own Metal
  renderer); the log says `gpu: driver metal (SDL_Renderer)`. Device and macOS are SDL_GPU.
- `platform/ios/Info.plist.in`: display name `ORBIS`, local networking allowed (ATS),
  local-network usage string, all orientations, `UIRequiresFullScreen` false, empty
  `UILaunchScreen`. No app icon yet.

## Android

The same C++ tree again: `listener` becomes `libmain.so`, loaded by SDL's `SDLActivity` beside
`libSDL3.so`. `make android` from the repo root writes a debug APK to
`listener/platform/android/app/build/outputs/apk/debug/app-debug.apk`.

Needs JDK 21 (`ANDROID_JDK`, Gradle 8.12 refuses 25) and an SDK at `ANDROID_HOME` with licenses
accepted; both default in the root Makefile to this machine's paths. Gradle installs the pinned NDK
(28.2.13676358, so 16 KB pages hold), build-tools and CMake 3.31.6 itself when they are missing:

```sh
brew install --cask android-commandlinetools && brew install openjdk@21
export JAVA_HOME=/opt/homebrew/opt/openjdk@21 ANDROID_HOME=/path/to/opt/android-sdk
yes | sdkmanager --sdk_root=$ANDROID_HOME --licenses
sdkmanager --sdk_root=$ANDROID_HOME platform-tools 'platforms;android-35'
```

To run it (arm64 image, Hypervisor.framework on Apple Silicon; `-gpu host` with a screen):

```sh
sdkmanager --sdk_root=$ANDROID_HOME emulator 'system-images;android-35;google_apis;arm64-v8a' 'cmdline-tools;latest'
$ANDROID_HOME/cmdline-tools/latest/bin/avdmanager create avd -n orbis -d pixel_6 \
    -k 'system-images;android-35;google_apis;arm64-v8a'
$ANDROID_HOME/emulator/emulator -avd orbis -no-window -no-audio -gpu swiftshader_indirect &
adb install -r listener/platform/android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n dev.orbis.listener/.OrbisActivity
adb logcat -s orbis            # util/Log.hpp mirrors to logcat
adb exec-out screencap -p > shot.png
```

The way in is pairing; no token is baked, by CMake or Gradle. A person on a listener takes a code
from DEVICES > PAIR PHONE, then either types it into Settings > PAIR WITH CODE or opens the link:

```sh
adb shell "am start -a android.intent.action.VIEW -d 'orbis://pair?url=ws%3A%2F%2F10.0.2.2%3A7777%2Fws&code=123456'"
```

The emulator reaches the host at `10.0.2.2`. The token lands in `SDL_GetPrefPath` (app-private,
`allowBackup` off), with prefs and `session.json`.

- `platform/android/` is SDL's `android-project` for `release-3.4.18`: AGP and the Gradle wrapper
  as pinned there, `org/libsdl/app/*.java` copied unchanged. Bumping SDL in
  `cmake/Dependencies.cmake` means copying those sources again; SDLActivity refuses a version
  mismatch at start, so a missed copy is loud. `dev/orbis/listener/` holds the only Java of ours.
- Rendering: SDL_GPU on Vulkan with the SPIR-V ImGui's backend ships. No device, or no window
  claim, falls back to GLES through SDL_Renderer; the log says `gpu: driver vulkan` or
  `gpu: driver opengles2 (SDL_Renderer)`. Force it with
  `<meta-data android:name="SDL_ENV.SDL_GPU_DRIVER" android:value="none" />` in the manifest.
- `src/platform/`: `Platform.hpp` defines `LISTENER_PHONE`, which selects the phone frame and
  lifecycle on iOS and Android alike. `Keyboard.hpp` is the keyboard height, from UIKit's
  notification on iOS and from `OrbisSurface`'s IME inset on Android.
- `OrbisSurface` also reports the safe area without the system gesture zones, which SDL includes
  and which would cost both sides of the screen under gesture navigation.
- Fonts ride as `assets/assets/fonts/*`, copied after CMake fetched them, and load through
  `SDL_LoadFile`, which opens relative paths through AssetManager. Themes are embedded.
- `arm64-v8a` only by default; `./gradlew assembleDebug -PorbisAbis=arm64-v8a,x86_64` adds PC
  emulators. Each ABI's CMake tree fetches its own dependencies under `app/.cxx`.

### Release signing

The keystore is the owner's and stays out of the repo; `assembleRelease` writes an unsigned APK.

```sh
keytool -genkeypair -v -keystore ~/orbis-release.jks -alias orbis -keyalg RSA -keysize 4096 -validity 10000
cd listener/platform/android && JAVA_HOME=/opt/homebrew/opt/openjdk@21 ./gradlew assembleRelease
BT=$ANDROID_HOME/build-tools/35.0.0
$BT/zipalign -p -f 4 app/build/outputs/apk/release/app-release-unsigned.apk orbis.apk
$BT/apksigner sign --ks ~/orbis-release.jks --ks-key-alias orbis orbis.apk
```

Play wants an app bundle from `bundleRelease`, signed with the same key.

## Drive API (agentic UX testing)

`listener --drive <unix-socket>` (or `LISTENER_DRIVE=<path>`) opens a JSON Lines control socket so
a script, or later a world-side agent, can drive the real window and read it back. Off by default;
desktop only. Each request `{"id":n,"cmd":"...",...}` is answered `{"id":n,"ok":true,...}` or
`{"id":n,"ok":false,"err":"..."}`. Commands run in order; a `wait` holds the ones behind it.

| cmd | args | does |
| --- | --- | --- |
| `screenshot` | `path` (.bmp) | renders the current frame to a BMP (the client converts to PNG with `sips`) |
| `state` | | window size and scale, attached + head, session principal, route state, the current Principal and its name, visible windows with rects, registered targets with rects, the current terminal's transcript as lines (`from_name`, `at`, `seq`, `text`, `mine`, `via`), `replies` count, the Evaluation state (`state`, tokens, ctx bytes, `progress`, `status`, `totals`, `stalled`), `input_text` and `input_focused`, the last notes, `carrier_lost` |
| `targets` | | the names `click` accepts right now |
| `type` | `text` | text input into the focused widget |
| `key` | `key`, `mods` | one key press: `Return Escape Up Down Left Right Tab Backspace Delete Space Home End PageUp PageDown Period Comma Minus Equal Slash a..z 0..9 F1..F9`, mods `cmd shift alt ctrl` |
| `click` | `target` or `x`,`y`; `times` | left click at the target's centre (window points) |
| `drag` | `from`, `to` (targets) or `to_x`,`to_y` | press, move in six steps, release |
| `scroll` | `target`, `dy`, `dx` | mouse wheel over the target |
| `drop` | `path` or `text`; `target` (default `input`) | drops a file or folder path, or text, onto the current terminal as Finder would (sprint 1 L8): a file is attached (an Object with `file/*` Fields, then a Message tagging it), a folder shows `[ MOUNT ] [ ATTACH LISTING ]`, text becomes a small text attachment; `state.attach` reports the progress |
| `wait` | `until`: `attached` `route` `settle` `idle` `reply` `text`; `text`; `timeout_ms`; `frames` | answers when the condition holds (`reply`: a new reply block for the current Principal since the wait began; `idle`: no running Evaluation and iteration 0; `text`: the transcript or notes contain the text) or `err timeout` with the state attached |
| `attach` | `url`, `token` or `token_file` | rebuild the runtime against another world |
| `focus` | | focus the current terminal's input line |
| `resize` | `w`, `h` | the window at a size (phone width on the desktop build; both at least 200) |
| `ping`, `quit` | | |

Targets: `pane:<TITLE>` (an ImGui window), `tab:<NAME>` (any slanted tab: TERMINAL, GRAPH,
TRANSCRIPT, PROMPT, FIELDS, a Principal's tab), `principal:<name>` (a PRINCIPALS row),
`button:<WORD>` (a bracket button: STOP, REASONING, SEND BIND OPS, ...), `link:<text>` (a linked symbol on screen), `input`, or `x,y`.
Approval cards: `button:approve:<task-short-id>`, `button:deny:<task-short-id>`,
`input:deny-reason:<task-short-id>`; `state.tasks` lists the pending Tasks. Dialogs:
`button:new-principal`, `input:create:<arg>` (nested as `grant.ops:<op>`, `grant.scope:<variant>`),
`button:create`, `button:mount`, `input:mount:<arg>`, `button:cancel`, `button:close`;
`button:actions` opens the terminal's menu and `menu:<action>` names its items (also on a graph
node's menu); `state.actions` lists the world's actions with their schemas.
Panes register them with one `drive::hit(name, min, max)` call per frame (`src/drive/Commands.hpp`);
the registry is empty and free when no socket is open.

Input is injected into ImGui's io one step per frame (a click is move, press, release over three
frames), so the UI reacts exactly as it would to a person. `screenshot` uses the same offscreen
render as `LISTENER_SHOT`.

Client: `tools/drive.py` (no dependencies), as a Python API or a CLI that runs commands in order:

```
tools/drive.py --socket /tmp/orbis.sock wait attached click tab:GRAPH wait settle screenshot graph.png
tools/drive.py --socket /tmp/orbis.sock click principal:root click input type "/fast hello" key Return wait reply --timeout 180000 state
tools/drive.py --socket /tmp/orbis.sock key w --mods cmd
```

The UX smoke `tests/ux/smoke_drive.py` (ctest `listener_ux`, label `ux`, skipped with 77 when no
world answers on 127.0.0.1:7777) launches the listener with `--drive`, waits for attach and the
route, screenshots, opens GRAPH, opens root's terminal, types a `/fast` question, waits for the
reply and its cost line, checks Escape clears a typed line, screenshots, quits.

These commands are the shape of the listener's future Adapter actions: once the listener is an
appliance Principal with a `listener` Adapter kind, a world-side Principal will `act` them over
its Route (spec 6.2, plan W3), and a UX test becomes a Task on the log like any other run.

Sprint 2 additions: `state.world` ({id name url head fresh_reason} from `hello`), targets
`button:world-menu`, `button:world-add`, `button:world-hello`, `button:world-attach:<url>`,
`tab:DEVICES`, `button:new-device`, `button:issue`, `button:revoke:<token8>`, `input:revoke-reason:<token8>`,
`button:pair-phone`, `button:pair-begin`, `text:pair-code`, `image:pair-qr`, `button:copy-secret`,
`button:save-secret`.

## Behaviour on the wire (docs/protocol.md)

- `attach {token principal? session? subs{sub: acked} client-kind listener}`; on `resumed true`
  nothing more is sent, on `resumed false` every sub is re-subscribed with `after = acked`.
- Pushes are deduplicated by `event/seq`, landed in the `EventStore` (which warns on
  `seq != last + 1`), and acknowledged with `{ack sub seq}`; the acked seq is persisted at once.
- `read {object}` for the principal (route discovery, `who am i`) and for every Message named by
  a `sent` / `delivered` event; `send {to route text tags}` from the input line.
- The main loop blocks in `SDL_WaitEventTimeout` until the next deadline (at most 250 ms); the
  socket thread posts an SDL user event when a frame arrives; frames are rendered only after
  events or deadlines. The screensaver hint is set before `SDL_Init`.

## What is stubbed

- `src/workspace/LocalFs.cpp`, `Shell.cpp`: only prepare `bind` ops naming the host as a workspace
  and a shell with placeholder object ids; the world answers `not-implemented` or
  `unresolved`. Real host binding (spec 15.6) is sprint 1 (`act create` for model/adapter).
- `tools/mockworld.cpp`: synthetic events, deterministic in seq, one every 500 ms; `read` answers
  for the principal (with a `principal/routes` Field naming its inbox Route), that Route and the
  Messages it created; `send` creates a Message and appends `sent` + `delivered`; no TTL, no
  resolution, no filters. It is for tests and the demo only.
- The History pane shows raw rows and chat lines, not presentation trees (spec 15.2).
- The streaming spinner has no event to react to until the world streams Evaluations.

Open contract questions are in `CONTRACT-NEEDS.md`.

### Targets and state added with the FIELDS/MEMORY work

- `hint:<field>` the HINT cell of a Field in FIELDS (click opens the editor, `type` + `Return` writes `<field>/hint`);
  `button:comments +` / `button:comments -` the object's thread; `button:create` the MEMORY tab's CREATE DEFAULTS;
  `button:edit:memory/notes`, `button:edit:memory/policy`, … and `input:memory/notes`, `input:memory/policy`, … the
  MEMORY editors; `tab:MEMORY` the view.
- `state.fields` the current Principal's Fields `{name: {value type by seq reader hint? comments?}}` (values clipped to
  300 chars); `state.boot` `{mode snapshot|replay|pending, seq, objects, pages, ms, fold_ready_ms, line}`;
  `state.link` `{reconnects, backoff_ms, stop_reason, attach_failures, attach_error, frames_in, frames_out,
  socket_opens, attaches, conn_state}`.
- `--data-dir <dir>` gives a second listener on the same Mac its own `session.json`; the UX smoke uses a temp dir.
  The world keeps one channel per session, so two instances sharing one id close each other on every attach: the
  listener detects three closes in ten seconds, backs off 1 s → 30 s and says so in SESSION's LINK row.

### Views and drops (sprint 1 L8/L10)

A Message of kind `view` names a Serialization Object (`view/kind` text | table | tree | graph | image,
`view/title`, `view/data` inline or a blob ref, `view/about`, `view/truncated?`); a Message that merely
tags an Object carrying `view/kind` renders the same way. The transcript shows it as a titled block:
the kind chip, the title, the `about:` refs as chips (click opens the object), `[via …]`, and the body
by kind. Tables sort by header click, resize their columns, clip their rows and copy as TSV
(right-click); a cell holding a ref opens that object. Trees fold with chamfered expanders. Graphs
pan and zoom inside their canvas; `[ EXPAND ]` puts them on the GRAPH pane as a cyan overlay with
`[ DISMISS ]`. Images decode once (stb_image) and open full size on click. Tables and images also
open in their own window from `[ EXPAND ]`.

Drag a file from Finder onto a terminal and it is attached to that Principal's conversation: an
Object with `file/name`, `file/mime`, `file/size`, `file/sha256` and `file/text` (text up to 64 KiB)
or `file/bytes` (bytes, base64 on the wire, blobbed by the world above 4 KiB), then a Message
`attached: <name>` on the Principal's route with the Object in `message/tags`. Files above 8 MiB are
refused with the reason. A folder asks `[ MOUNT ]` (`workspace.mount` for that Principal) or
`[ ATTACH LISTING ]` (its top two levels as a tree view). Progress shows above the input line.

Drive targets added: `view:table:<id8>`, `view:tree:<id8>`, `view:graph:<id8>`, `view:image:<id8>`,
`button:expand:<id8>`, `button:fit:<id8>`, `button:close:<id8>`, `button:drop-mount`,
`button:drop-listing`, `button:drop-cancel`, `button:attach-dismiss`, `button:dismiss-overlay`;
state keys: transcript lines carry `kind`, `tags` and `view {id kind title about truncated ready}`,
plus `attach {stage err done}` and `graph_overlay`.
