FetchContent cache lives in _deps/ (git-ignored). Pins are in cmake/Dependencies.cmake.

stb/stb_image.h: stb_image v2.30 (public domain / MIT), nothings/stb commit 013ac3beddff3dbffafd5177e7972067cd2b5083
(2024-05-31), sha256 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3. Used by ui/Textures.cpp
to decode PNG/JPEG for image views (sprint 1 L10). License text is at the end of the file.
libvterm: neovim/libvterm commit 934bc2fbf21800ac3458a499df8820ca5fb45fd3 (2025-11-20, 0.3.3 line), MIT. Plain C, pinned in
cmake/Dependencies.cmake. Used by modules/TerminalScreen for the terminal module. Its tables are committed in its
repository, so no perl step at build time; it builds the same for iOS and Android.

Fonts: JetBrains Mono, Inter and Roboto Condensed (headings), all OFL, fetched by cmake/Fonts.cmake from one pinned
google/fonts commit into assets/fonts.
