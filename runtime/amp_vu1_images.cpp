// The compiled VU1 programs (runtime/amp_vu.h's Image table): none yet. Every VU1 program runs in the interpreter of
// runtime/amp_vu1.cpp; a recompiler for the executable's microcode packets (Shinobido's tools/shino_vu1_recomp.py) can
// fill this table later, when the programs' cost shows.
#include "amp_vu.h"

namespace amp::vu
{
    const Image kImages[1] = {};
    const uint32_t kImageCount = 0;
}
