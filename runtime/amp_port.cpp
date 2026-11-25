// Amplitude's native port boundary. No hardware execution or interpreter fallback.
//
// Overrides: each registerFunction(0x...) below is listed for bozo-externals by tools/amp_overrides_list.py, so a direct
// call in the generated code reaches the port's function. Native instruction sites: each `case 0x...u: // native site`
// line is listed as a "native 0xADDR" external: the instruction is replaced by the handler, inline.
// docs/RECOMP_NOTES.md says why each one is there.
#include "amp_port.h"
#include "bozo/ee/dispatcher.h"
#include <cstring>

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

        void returnZero(uint8_t *, R5900Context *ctx, Runtime *)
        {
            ctx->r[2].ud[0] = 0;
            ctx->pc = ctx->r[31].uw[0];
        }

        bool nativeInstruction(uint8_t *, R5900Context *, Runtime *, uint32_t pc)
        {
            switch (pc)
            {
            default:
                return false;
            }
        }
    } // namespace

    void install(Runtime &, bozo::port::Registry &r)
    {
        r.registerFunction(0x31b0b8, ioctl); // sceIoctl
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
        r.nativeGraphics(&nativeInstruction);
    }
} // namespace amp
