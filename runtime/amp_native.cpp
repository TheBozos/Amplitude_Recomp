// Amplitude's native renderer (in progress): Harmonix's draws answered at their own functions, their meshes, materials
// and matrices drawn through BozoEngine's 3D path instead of the VIF1 / VU1 / GIF packets they build.
// docs/RECOMP_NOTES.md ("Native renderer") maps the classes and functions read so far.
//
// The PS2 mesh's draw, 0x215f30 (a virtual method: vtable 0x3d2a70, slot 5): a visibility test 0x232b30, the skinning
// 0x215d18 (the world transform it returns), then for each of the material's passes the material's GS state
// (0x213698, the default material 0x213570) and the VU1 header and prebuilt packet (0x2155b0).
#include "amp_functions.h"
#include "amp_gs.h"
#include "amp_port.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace amp
{
    namespace
    {
        // AMP_TRACE_MESHES=<flip>: each mesh drawn at that flip (its object, once), then the game's own draw.
        void meshDraw(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            static const char *trace = std::getenv("AMP_TRACE_MESHES");
            if (trace && gs::flips() == std::strtoull(trace, nullptr, 10))
            {
                static std::set<uint32_t> seen;
                const uint32_t self = ctx->r[4].uw[0];
                if (seen.insert(self).second)
                {
                    std::printf("[amp:mesh] %08x:", self);
                    for (uint32_t o = 0; o < 0x1c0; o += 4)
                    {
                        uint32_t v;
                        std::memcpy(&v, ram + ((self + o) & 0x1FFFFFFu), 4);
                        std::printf(" %08x", v);
                    }
                    std::printf("\n");
                }
            }
            func_0x215f30(ram, ctx, rt);
        }
    } // namespace

    void installNative(Runtime &, bozo::port::Registry &r)
    {
        r.registerFunction(0x215f30, meshDraw); // the PS2 mesh's draw
    }
} // namespace amp
