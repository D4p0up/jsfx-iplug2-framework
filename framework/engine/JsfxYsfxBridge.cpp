#include "JsfxYsfxBridge.h"
#include "ysfx.hpp"

void jsfx::YsfxRequestSliderSection(ysfx_t* fx)
{
  fx->must_compute_slider = true;
}
