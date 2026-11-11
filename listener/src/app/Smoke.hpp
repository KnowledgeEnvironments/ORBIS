// --smoke N: run Runtime against a world for N seconds with no SDL, drop the connection halfway,
// and print a summary. Exit code 0 means: attached, rows streamed before and after the drop, the
// drop was resumed, and the EventStore saw no gap. This is the verification path for a headless
// machine; the windowed binary is the same stack plus SDL.
#pragma once

#include "app/Options.hpp"

namespace ke {

int runSmoke(const Options& opts);

}
