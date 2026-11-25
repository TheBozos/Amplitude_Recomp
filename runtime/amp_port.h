// Amplitude's native port boundary.
#pragma once
#include "bozo/port.h"

namespace amp
{
    // The port's overrides (registerFunction by address) and native instruction sites (runtime/amp_port.cpp).
    void install(Runtime &rt, bozo::port::Registry &registry);
} // namespace amp
