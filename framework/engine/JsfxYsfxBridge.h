#pragma once
// Small additions to the ysfx API that need its internals (ysfx.hpp).
// Implemented in JsfxYsfxBridge.cpp, which is compiled into jsfx_core.
#include "ysfx.h"

namespace jsfx {
/** Makes the next ysfx_process_*() run @slider, without touching any slider
    value (safe while @gfx is writing sliders on another thread). */
void YsfxRequestSliderSection(ysfx_t* fx);
}
