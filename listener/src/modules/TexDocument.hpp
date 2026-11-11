// A .tex document as Markdown, for the view that needs no model: sections, lists, emphasis and
// math come through, the preamble goes. The page itself is the world's `render`; this is what a
// client shows when the world has no model, or before the first render lands.
#pragma once

#include <string>
#include <string_view>

namespace ke::modules {

std::string texToMarkdown(std::string_view tex);

} // namespace ke::modules
