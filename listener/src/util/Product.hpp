// The product's name, once. A rename changes this line, MACOSX_BUNDLE_BUNDLE_NAME and the Android
// app_name. The name also places the data directory, so a rename moves where prefs are looked for:
// prefs under the old name are not migrated.
#pragma once

namespace ke {

inline constexpr const char* kProductName = "ORBIS";
inline constexpr const char* kProductExpansion = "On-line Relative Basis Information System";
inline constexpr const char* kTagline = "A shared knowledge environment for doing work with people, computers, and artificial intelligence.";

} // namespace ke
