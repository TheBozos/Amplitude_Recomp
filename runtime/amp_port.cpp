// Amplitude's native port boundary. No hardware execution or interpreter fallback.
//
// Overrides: each registerFunction(0x...) below is listed for bozo-externals by tools/amp_overrides_list.py, so a direct
// call in the generated code reaches the port's function. Native instruction sites: each `case 0x...u: // native site`
// line is listed as a "native 0xADDR" external: the instruction is replaced by the handler, inline.
// docs/RECOMP_NOTES.md says why each one is there.
#include "amp_port.h"
#include "amp_ark_overlay.h"
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
            // The archive's first sector, for the overlay's reads (amp_ark_overlay.h).
            if (search && ctx->r[2].sw[0] == 1 && path.find("MAIN.ARK") != std::string::npos)
                ark::setArchiveSector(g.read32(g.arg(0)));
            static const bool on = std::getenv("AMP_TRACE_FILES") != nullptr;
            if (on)
                std::printf("[amp:file] %s(\"%s\"%s) = %d (from %x)\n", Name, path.c_str(),
                        search ? "" : (std::string(", ") + std::to_string(g.arg(1))).c_str(), ctx->r[2].sw[0], ra);
            ctx->pc = ra;
        }
        constexpr char kSceOpen[] = "sceOpen", kSceGetstat[] = "sceGetstat", kSceCdSearchFile[] = "sceCdSearchFile";

        // sceCdRead(lbn, sectors, buffer, mode): the engine's answer; AMP_TRACE_FILES=1 prints the sectors read.
        void traceCdRead(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            bozo::libs::Libraries *l = libraries();
            const bozo::libs::Function *f = l ? l->find("sceCdRead") : nullptr;
            bozo::libs::Guest g(*rt, ctx);
            const uint32_t lbn = g.arg(0), n = g.arg(1), to = g.arg(2), ra = ctx->r[31].uw[0];
            if (f)
                f->handler(*l, g);
            // The archive overlay's files (amp_ark_overlay.h) laid over the sectors read.
            if (ctx->r[2].sw[0] == 1)
                ark::overlay(ram, lbn, n, to);
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
            return renderSite(ram, ctx, rt, pc) || ipuSite(ram, ctx, rt, pc) || vu0Site(ram, ctx, rt, pc);
        }
    } // namespace

    void install(Runtime &rt, bozo::port::Registry &r)
    {
        installRender(rt, r);
        installIpu(rt, r);
        installMenu(r);
        installNative(rt, r);
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
        // The network configuration's clients (online play, out of scope): the set-up 0x253438 calls 0x26c648
        // (eznetcnf: sceSifSearchModuleByName, then a bind loop on EZNCNF_S's server 0x75499128) and 0x26c8f8
        // (eznetctl, EZNCTL_S's 0x75488909), and ignores what they return. Neither module is loaded: both do nothing,
        // their ready flags stay clear.
        r.registerFunction(0x26c648, returnZero);
        r.registerFunction(0x26c8f8, returnZero);
        r.nativeGraphics(&nativeInstruction);
    }
} // namespace amp
