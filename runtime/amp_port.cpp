// Amplitude's native port boundary. No hardware execution or interpreter fallback.
//
// Overrides: each registerFunction(0x...) below is listed for bozo-externals by tools/amp_overrides_list.py, so a direct
// call in the generated code reaches the port's function. Native instruction sites: each `case 0x...u: // native site`
// line is listed as a "native 0xADDR" external: the instruction is replaced by the handler, inline.
// docs/RECOMP_NOTES.md says why each one is there.
#include "amp_port.h"
#include "bozo/ee/dispatcher.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "bozo/libs/guest.h"

namespace amp
{
    namespace
    {
        // sceIoctl(fd, request, arg) (0x31b0b8, the fileio client: RPC function 5). The game opens its files with
        // O_NOWAIT (0x8001) and polls request 1 into *arg until it is no longer 1 (busy): the file layer 0x1af6a0
        // after sceOpen, the reads 0x28f528/0x28f598/0x28f6b0 after sceRead. The engine's sceOpen/sceRead finish before
        // they return, so every operation is already done: *arg = 0. No other request is used by the game.
        void ioctl(uint8_t *ram, R5900Context *ctx, Runtime *)
        {
            const uint32_t request = ctx->r[5].uw[0], arg = ctx->r[6].uw[0];
            int32_t result = -1;
            if (request == 1 && arg)
            {
                const uint32_t done = 0;
                std::memcpy(ram + (arg & 0x1FFFFFFu), &done, 4);
                result = 0;
            }
            ctx->r[2].sd[0] = result;
            ctx->pc = ctx->r[31].uw[0];
        }

        // The file system's calls (sceOpen, sceGetstat, sceCdSearchFile): the engine's own answer; AMP_TRACE_FILES=1
        // prints each one's path and result.
        template <const char *Name> void traceFile(uint8_t *, R5900Context *ctx, Runtime *rt)
        {
            bozo::libs::Libraries *l = libraries();
            const bozo::libs::Function *f = l ? l->find(Name) : nullptr;
            bozo::libs::Guest g(*rt, ctx);
            const bool search = std::strcmp(Name, "sceCdSearchFile") == 0;
            const std::string path = g.string(search ? g.arg(1) : g.arg(0));
            const uint32_t ra = ctx->r[31].uw[0];
            if (f)
                f->handler(*l, g);
            static const bool on = std::getenv("AMP_TRACE_FILES") != nullptr;
            if (on)
                std::printf("[amp:file] %s(\"%s\"%s) = %d (from %x)\n", Name, path.c_str(),
                        search ? "" : (std::string(", ") + std::to_string(g.arg(1))).c_str(), ctx->r[2].sw[0], ra);
            ctx->pc = ra;
        }
        constexpr char kSceOpen[] = "sceOpen", kSceGetstat[] = "sceGetstat", kSceCdSearchFile[] = "sceCdSearchFile";

        // sceCdRead(lbn, sectors, buffer, mode): the engine's answer; AMP_TRACE_FILES=1 prints the sectors read.
        void traceCdRead(uint8_t *, R5900Context *ctx, Runtime *rt)
        {
            bozo::libs::Libraries *l = libraries();
            const bozo::libs::Function *f = l ? l->find("sceCdRead") : nullptr;
            bozo::libs::Guest g(*rt, ctx);
            const uint32_t lbn = g.arg(0), n = g.arg(1), to = g.arg(2), ra = ctx->r[31].uw[0];
            if (f)
                f->handler(*l, g);
            static const bool on = std::getenv("AMP_TRACE_FILES") != nullptr;
            if (on)
                std::printf("[amp:file] sceCdRead(lbn %u, %u sectors, to %x) = %d (from %x)\n", lbn, n, to, ctx->r[2].sw[0], ra);
            ctx->pc = ra;
        }

        void returnZero(uint8_t *, R5900Context *ctx, Runtime *)
        {
            ctx->r[2].ud[0] = 0;
            ctx->pc = ctx->r[31].uw[0];
        }

        // Native instruction sites: the renderer's DMA register accesses (runtime/amp_render.cpp).
        bool nativeInstruction(uint8_t *ram, R5900Context *ctx, Runtime *rt, uint32_t pc)
        {
            return renderSite(ram, ctx, rt, pc) || ipuSite(ram, ctx, rt, pc);
        }
    } // namespace

    void install(Runtime &rt, bozo::port::Registry &r)
    {
        installRender(rt, r);
        installIpu(rt, r);
        r.registerFunction(0x31b0b8, ioctl); // sceIoctl
        r.registerFunction(0x31a558, traceFile<kSceOpen>);
        r.registerFunction(0x31b610, traceFile<kSceGetstat>);
        r.registerFunction(0x314330, traceFile<kSceCdSearchFile>);
        r.registerFunction(0x315088, traceCdRead); // sceCdRead
        // The USB keyboard (libusbkb, USBKB.IRX's RPC server 0x80000211) serves online chat only. The game's keyboard
        // init 0x28b148 (sceUsbKbInit 0x310618 binds the server in a loop that never ends without the module), its end
        // 0x28b1c8 and its per-frame poll 0x28b1f0 do nothing: no keyboard is ever connected.
        r.registerFunction(0x28b148, returnZero);
        r.registerFunction(0x28b1c8, returnZero);
        r.registerFunction(0x28b1f0, returnZero);
        // sceIpuInit (0x30efb0: IPU_CTRL = 0x40000000, the reset, then a wait on its busy bit). Called by libmpeg's
        // sceMpegInit (native) and by the game's own IPU decoder set-up 0x2163c0, which then installs its DMA channel 3
        // (from IPU) handler. There is no IPU to reset; what the game then asks of the IPU is answered where it asks.
        r.registerFunction(0x30efb0, returnZero);
        // sceSifSetDChain (0x317300, syscall 120): the SIF DMA chain armed before a sceSifSetDma (native: the data
        // reaches the IOP modules at once). Nothing to arm. (Onimusha_Recomp answers it the same way.)
        r.registerFunction(0x317300, returnZero);
        r.registerFunction(0x317310, returnZero); // isceSifSetDChain
        // sceSifInitCmd (0x318c28): the SIF command layer's set-up, called directly by the game's sound set-up
        // 0x24d2e0 (and by sceSifInitRpc, native). The engine's SIF has nothing to set up (its sceSifInitRpc and
        // sceSifExitCmd do nothing either).
        r.registerFunction(0x318c28, returnZero);
        // The network configuration's clients (online play, out of scope): the set-up 0x253438 calls 0x26c648
        // (eznetcnf: sceSifSearchModuleByName, then a bind loop on EZNCNF_S's server 0x75499128) and 0x26c8f8
        // (eznetctl, EZNCTL_S's 0x75488909), and ignores what they return. Neither module is loaded: both do nothing,
        // their ready flags stay clear.
        r.registerFunction(0x26c648, returnZero);
        r.registerFunction(0x26c8f8, returnZero);
        r.nativeGraphics(&nativeInstruction);
    }
} // namespace amp
