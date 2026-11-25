// Amplitude's renderer: its DMA channels, the MFIFO ring and the display flip, native.
//
// Harmonix's renderer (docs/RECOMP_NOTES.md, "Drawing") wraps each DMA channel in a small object (+4 the packet's base,
// +8 its cursor, +0x14 the channel) and programs the channel through tables of register addresses (0x3b06a0 CHCR,
// 0x3b06c8 MADR, 0x3b06f0 QWC, 0x3b0718 TADR, 0x3b0740 SADR): 0x212328 sets them, 0x212500 writes CHCR (the start),
// 0x2122e8 waits for STR to clear. Packets are built in the scratchpad and sent to a 512 KB ring in RAM by the
// from-scratchpad channel (8) in MFIFO mode (the renderer's init 0x210bb8: D_RBOR, D_RBSR = 0x7fff0, D_CTRL.MFD = 2),
// and VIF1 (channel 1, chain mode) drains the ring up to channel 8's MADR: the frame's kick 0x211410 starts it at the
// ring's write position. The VU0 and VU1 microcode is uploaded at init by VIF0 and VIF1 chains (TTE) from .vutext.
//
// There is no DMAC, VIF or GIF: every load and store of the renderer's to a channel register is a native site here
// (case 0x...u: // native site, listed for bozo-externals by tools/amp_overrides_list.py), on a register file of the
// port's. A transfer runs when its CHCR is started, at once: VIF0 / VIF1 codes go to the VU interpreters
// (runtime/amp_vu1.cpp), GIF packets to the GS translator (runtime/amp_gs.cpp), scratchpad copies are copies (into the
// ring, wrapping, in MFIFO mode), and VIF1 in MFIFO mode follows the ring's tags up to channel 8's MADR. The flip
// 0x21b248 (an inlined sceGsPutDispEnv of one of two display environments) publishes the frame.
#include "amp_gs.h"
#include "amp_port.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/render/renderer.h"
#include "bozo/runtime_api.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace amp
{
    namespace
    {
        uint8_t *g_ram = nullptr, *g_spr = nullptr;
        Runtime *g_rt = nullptr;

        // A guest address: RAM (any segment) or the scratchpad (0x70000000, or a DMA address with the SPR bit).
        uint8_t *at(uint32_t a)
        {
            if ((a & 0xF0000000u) == 0x70000000u || (a & 0x80000000u) && (a & 0x70000000u) == 0)
                return g_spr + (a & 0x3FF0u) + (a & 0xFu);
            return g_ram + (a & 0x1FFFFFFu);
        }
        uint64_t rd64(uint32_t a) { uint64_t v; std::memcpy(&v, at(a), 8); return v; }

        bool traceDma() { static const bool on = std::getenv("AMP_TRACE_DMA") != nullptr; return on; }

        // ---- The port's DMA register file --------------------------------------------------------------------------
        struct Channel
        {
            uint32_t chcr = 0, madr = 0, qwc = 0, tadr = 0, sadr = 0;
        };
        std::array<Channel, 10> g_ch;
        uint32_t g_ctrl = 0, g_rbor = 0, g_rbsr = 0;
        // VIF1 draining the ring (MFIFO): its tag address, and whether it runs.
        uint32_t g_vif1Tadr = 0;
        bool g_vif1Mfifo = false;
        uint64_t g_transfers[10] = {}, g_ringBytes = 0, g_ringTags = 0;

        int channelOf(uint32_t address)
        {
            switch (address & 0xFFFFFF00u)
            {
            case 0x10008000u: return 0;
            case 0x10009000u: return 1;
            case 0x1000A000u: return 2;
            case 0x1000B000u: return 3;
            case 0x1000B400u: return 4;
            case 0x1000C000u: return 5;
            case 0x1000C400u: return 6;
            case 0x1000C800u: return 7;
            case 0x1000D000u: return 8;
            case 0x1000D400u: return 9;
            default: return -1;
            }
        }
        bool mfifo() { return ((g_ctrl >> 2) & 3u) == 2u; }
        uint32_t ringAddress(uint32_t a) { return g_rbor | (a & g_rbsr); }

        // A source-chain transfer, its data handed to `take(bytes pointer, count, tagWords)` in order. The tags: refe 0,
        // cnt 1, next 2, ref 3, refs 4, call 5, ret 6, end 7 (two call levels). With TTE the tag's upper 64 bits are two
        // VIF codes sent before the data. (Shinobido_WOTN_Recomp's walkChain.)
        template <class Take> void walkChain(uint32_t tadr, bool tte, Take &&take)
        {
            uint32_t stack[2] = {}, depth = 0;
            for (int guard = 0; guard < 200000; ++guard)
            {
                const uint64_t tag = rd64(tadr);
                const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu), id = static_cast<uint32_t>(tag >> 28) & 7u;
                const uint32_t addr = static_cast<uint32_t>(tag >> 32) & 0xFFFFFFF0u;
                if (tte)
                    take(at(tadr + 8), 8u, true);
                switch (id)
                {
                case 0: take(at(addr), qwc * 16u, false); return;
                case 1: take(at(tadr + 16), qwc * 16u, false); tadr += 16 + qwc * 16; break;
                case 2: take(at(tadr + 16), qwc * 16u, false); tadr = addr; break;
                case 3: case 4: take(at(addr), qwc * 16u, false); tadr += 16; break;
                case 5:
                    take(at(tadr + 16), qwc * 16u, false);
                    if (depth >= 2) return;
                    stack[depth++] = tadr + 16 + qwc * 16;
                    tadr = addr;
                    break;
                case 6:
                    take(at(tadr + 16), qwc * 16u, false);
                    if (!depth) return;
                    tadr = stack[--depth];
                    break;
                default: take(at(tadr + 16), qwc * 16u, false); return;
                }
            }
            std::fprintf(stderr, "[amp:dma] chain at %x did not end\n", tadr);
        }

        // VIF1 in MFIFO mode: its tags follow one another in the ring (cnt data after the tag, wrapping at the ring's
        // end; next jumps within the ring), ref/refs point anywhere; it stops when its tag address reaches channel 8's
        // MADR (the ring is drained) or at an end tag.
        void drainRing()
        {
            static std::vector<uint8_t> span;
            const uint32_t limit = g_ch[8].madr;
            for (int guard = 0; g_vif1Mfifo && guard < 1000000; ++guard)
            {
                g_ch[1].tadr = g_vif1Tadr; // D1_TADR as the flush's free-space wait reads it (0x211128)
                const uint32_t tadr = ringAddress(g_vif1Tadr);
                if (tadr == ringAddress(limit))
                    return;
                const uint64_t tag = rd64(tadr);
                const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu), id = static_cast<uint32_t>(tag >> 28) & 7u;
                const uint32_t addr = static_cast<uint32_t>(tag >> 32) & 0xFFFFFFF0u;
                const bool tte = (g_ch[1].chcr & 0x40u) != 0;
                ++g_ringTags;
                if (tte)
                    vif::codes(at(tadr + 8), 8, true);
                auto inRing = [&](uint32_t from) {
                    // qwc quadwords from ring address `from`, wrapping at the ring's end.
                    span.resize(size_t(qwc) * 16);
                    for (uint32_t q = 0; q < qwc; ++q)
                        std::memcpy(span.data() + q * 16, at(ringAddress(from + q * 16)), 16);
                    g_ringBytes += span.size();
                    vif::codes(span.data(), qwc * 16u, false);
                };
                switch (id)
                {
                case 1: inRing(tadr + 16); g_vif1Tadr = ringAddress(tadr + 16 + qwc * 16); break; // cnt
                case 2: inRing(tadr + 16); g_vif1Tadr = addr; break;                               // next
                case 3: case 4: vif::codes(at(addr), qwc * 16u, false); g_vif1Tadr = ringAddress(tadr + 16); break;
                case 7: inRing(tadr + 16); g_vif1Mfifo = false; g_ch[1].chcr &= ~0x100u; return; // end
                case 0: vif::codes(at(addr), qwc * 16u, false); g_vif1Mfifo = false; g_ch[1].chcr &= ~0x100u; return;
                default:
                    std::fprintf(stderr, "[amp:dma] VIF1 MFIFO tag %u at %x: not followed\n", id, tadr);
                    g_vif1Mfifo = false;
                    g_ch[1].chcr &= ~0x100u;
                    return;
                }
            }
        }

        // A channel started (CHCR written with STR): the transfer runs at once.
        void start(int c)
        {
            Channel &ch = g_ch[c];
            const uint32_t mode = (ch.chcr >> 2) & 3u;
            const bool tte = (ch.chcr & 0x40u) != 0;
            ++g_transfers[c];
            if (traceDma())
                std::printf("[amp:dma] ch%d chcr=%x madr=%x qwc=%x tadr=%x sadr=%x ctrl=%x\n", c, ch.chcr, ch.madr, ch.qwc,
                            ch.tadr, ch.sadr, g_ctrl);
            switch (c)
            {
            case 0: // VIF0
                if (mode == 1)
                    walkChain(ch.tadr, tte, [](const uint8_t *p, uint32_t n, bool) { vif::codes0(g_rt, p, n); });
                else
                    vif::codes0(g_rt, at(ch.madr), ch.qwc * 16u);
                break;
            case 1: // VIF1
                if (mode == 1 && mfifo())
                {
                    g_vif1Tadr = ch.tadr;
                    g_vif1Mfifo = true;
                    drainRing();
                    return; // STR stays set until the ring's end tag
                }
                if (mode == 1)
                    walkChain(ch.tadr, tte, [](const uint8_t *p, uint32_t n, bool w) { vif::codes(p, n, w); });
                else
                    vif::codes(at(ch.madr), ch.qwc * 16u, false);
                break;
            case 2: // GIF (PATH3)
                if (mode == 1)
                    walkChain(ch.tadr, false, [](const uint8_t *p, uint32_t n, bool) { gs::packets(p, n); });
                else
                    gs::packets(at(ch.madr), ch.qwc * 16u);
                break;
            case 8: // from the scratchpad: SADR (scratchpad) to MADR (the ring in MFIFO mode)
                for (uint32_t q = 0; q < ch.qwc; ++q)
                {
                    const uint32_t to = mfifo() ? ringAddress(ch.madr) : ch.madr;
                    std::memcpy(at(to), g_spr + ((ch.sadr + q * 16) & 0x3FF0u), 16);
                    ch.madr = (mfifo() ? ringAddress(ch.madr + 16) : ch.madr + 16);
                }
                ch.sadr += ch.qwc * 16;
                ch.qwc = 0;
                ch.chcr &= ~0x100u;
                if (g_vif1Mfifo)
                    drainRing();
                return;
            case 9: // to the scratchpad: MADR (RAM) to SADR (scratchpad)
                for (uint32_t q = 0; q < ch.qwc; ++q)
                    std::memcpy(g_spr + ((ch.sadr + q * 16) & 0x3FF0u), at(ch.madr + q * 16), 16);
                ch.madr += ch.qwc * 16;
                ch.sadr += ch.qwc * 16;
                ch.qwc = 0;
                break;
            default:
                std::fprintf(stderr, "[amp:dma] channel %d started (chcr %x): not answered\n", c, ch.chcr);
                break;
            }
            if (mode != 1)
            {
                ch.madr += ch.qwc * 16;
                ch.qwc = 0;
            }
            ch.chcr &= ~0x100u;
        }

        uint32_t readRegister(uint32_t address)
        {
            if (address == 0x1000E000u) return g_ctrl;
            if (address == 0x1000E010u) return 0; // D_STAT: nothing pending
            if (address == 0x1000E040u) return g_rbsr;
            if (address == 0x1000E050u) return g_rbor;
            const int c = channelOf(address);
            if (c < 0) return 0;
            const Channel &ch = g_ch[c];
            switch (address & 0xF0u)
            {
            case 0x00: return ch.chcr;
            case 0x10: return ch.madr;
            case 0x20: return ch.qwc;
            case 0x30: return ch.tadr;
            case 0x80: return ch.sadr;
            default: return 0;
            }
        }

        void writeRegister(uint32_t address, uint32_t value, uint32_t size)
        {
            if ((address & ~3u) == 0x1000E000u) // D_CTRL (the renderer writes its low byte: MFD)
            {
                if (size == 1)
                    g_ctrl = (g_ctrl & ~0xFFu) | (value & 0xFFu);
                else
                    g_ctrl = value;
                return;
            }
            if (address == 0x1000E040u) { g_rbsr = value; return; }
            if (address == 0x1000E050u) { g_rbor = value; return; }
            const int c = channelOf(address);
            if (c < 0) return;
            Channel &ch = g_ch[c];
            switch (address & 0xF0u)
            {
            case 0x00:
                ch.chcr = value;
                if (value & 0x100u)
                    start(c);
                break;
            case 0x10: ch.madr = value; break;
            case 0x20: ch.qwc = value & 0xFFFFu; break;
            case 0x30: ch.tadr = value; break;
            case 0x80: ch.sadr = value; break;
            default: break;
            }
        }

        // The flip: 0x21b248(env, buffer, field) writes PMODE (EN2 always, EN1 by `field`), SMODE2, DISPFB2, DISPLAY2,
        // BGCOLOR, DISPFB1, DISPLAY1 of one of the two display environments (+0x00 or +0x40, by `buffer`). The frame
        // drawn so far is published with circuit 2's buffer, and the game frame ends.
        void flip(uint8_t *, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t env = ctx->r[4].uw[0] + (ctx->r[5].uw[0] ? 0x40u : 0u);
            ctx->pc = ctx->r[31].uw[0];
            gs::flip(rt, ctx, rd64(env + 0x10), rd64(env + 0x18));
        }
        // sceGsPutDispEnv(disp) (0x30f7b0): the movie player 0x1aec48 shows each picture with it (PMODE, SMODE2,
        // DISPFB, DISPLAY, BGCOLOR at +0, +8, +0x10, +0x18, +0x20): a flip, as 0x21b248 is the renderer's.
        void putDispEnv(uint8_t *, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t disp = ctx->r[4].uw[0];
            ctx->r[2].ud[0] = 0;
            ctx->pc = ctx->r[31].uw[0];
            gs::flip(rt, ctx, rd64(disp + 0x10), rd64(disp + 0x18));
        }

        // sceGsPutDrawEnv(packet) (0x30fb60): the drawing environment (FRAME, ZBUF, XYOFFSET, SCISSOR, ...), a GIF tag
        // and its A+D pairs, sent through PATH3. The renderer selects the frame buffer to draw into with it each frame
        // (0x21b398): its packet goes to the GS translator, as the GIF would take it. Returns 0.
        void putDrawEnv(uint8_t *, R5900Context *ctx, Runtime *)
        {
            const uint32_t packet = ctx->r[4].uw[0];
            const uint64_t tag = rd64(packet);
            const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu), nreg = static_cast<uint32_t>(tag >> 60) ? static_cast<uint32_t>(tag >> 60) : 16u;
            gs::packets(at(packet), 16u * (1u + nloop * nreg));
            ctx->r[2].ud[0] = 0;
            ctx->pc = ctx->r[31].uw[0];
        }

        // A VU0 microprogram (VCALLMS) at its entry pair, in the interpreter.
        template <uint32_t kEntry> void vu0Program(uint8_t *, R5900Context *ctx, Runtime *rt) { vu0::run(rt, ctx, kEntry * 8u); }
    } // namespace

    // A load or store of the renderer's to a retired DMA register (the sites listed below): decoded and done on the
    // port's register file. The sites: the init's ring set-up (0x210bb8), the scratchpad flush's wait for room in the
    // ring (0x211128), the frame kick (0x211410), the readback's
    // MFIFO switch (0x211c88), the channel helpers (0x2122e8 wait, 0x212328 set-up, 0x212500 start), the GIF kick
    // 0x1b0888.
    bool renderSite(uint8_t *, R5900Context *ctx, Runtime *, uint32_t pc)
    {
        switch (pc)
        {
        case 0x210e0cu: // native site: D_RBOR
        case 0x210e18u: // native site: D_RBSR
        case 0x210e28u: // native site: D_CTRL read
        case 0x210e48u: // native site: D_CTRL (MFD = VIF1)
        case 0x210e54u: // native site: D1_TADR
        case 0x210e60u: // native site: D8_MADR
        case 0x211198u: // native site: D1_TADR read (the flush's wait for room in the ring)
        case 0x21119cu: // native site: D8_MADR read
        case 0x211440u: // native site: D8_MADR read (the frame kick)
        case 0x211450u: // native site: D1_TADR
        case 0x211460u: // native site: D1_CHCR (VIF1 chain start: the ring drained)
        case 0x211d04u: // native site: D_CTRL read
        case 0x211d0cu: // native site: D_CTRL (MFIFO off)
        case 0x211e0cu: // native site: D_CTRL read
        case 0x211e18u: // native site: D_CTRL (MFIFO on)
        case 0x212300u: // native site: CHCR read (the wait for STR)
        case 0x2123b4u: // native site: QWC
        case 0x2123d8u: // native site: MADR
        case 0x2123f4u: // native site: MADR
        case 0x212420u: // native site: SADR
        case 0x21244cu: // native site: SADR
        case 0x212498u: // native site: QWC
        case 0x2124acu: // native site: TADR
        case 0x2124d0u: // native site: SADR
        case 0x212534u: // native site: CHCR (the start)
        case 0x1b08a0u: // native site: D2_TADR
        case 0x1b08a8u: // native site: D2_QWC
        case 0x1b08b8u: // native site: D2_CHCR (GIF chain start)
            break;
        default:
            return false;
        }
        uint32_t word;
        std::memcpy(&word, g_ram + pc, 4);
        const uint32_t op = word >> 26, base = (word >> 21) & 31u, rt = (word >> 16) & 31u;
        const int32_t offset = static_cast<int16_t>(word & 0xFFFFu);
        const uint32_t address = ctx->r[base].uw[0] + static_cast<uint32_t>(offset);
        switch (op)
        {
        case 0x23: ctx->r[rt].sd[0] = static_cast<int32_t>(readRegister(address)); break;  // lw
        case 0x24: ctx->r[rt].ud[0] = readRegister(address & ~3u) >> (8 * (address & 3u)) & 0xFFu; break; // lbu
        case 0x2b: writeRegister(address, ctx->r[rt].uw[0], 4); break;                     // sw
        case 0x28: writeRegister(address, ctx->r[rt].uw[0] & 0xFFu, 1); break;             // sb
        default:
            std::fprintf(stderr, "[amp:dma] native site %x: instruction %08x not handled\n", pc, word);
            return false;
        }
        return true;
    }

    void installRender(Runtime &rt, bozo::port::Registry &r)
    {
        g_rt = &rt;
        g_ram = rt.memory.ram();
        g_spr = rt.memory.scratchpad();
        vif::eeRam = g_ram;
        r.registerFunction(0x21b248, flip);
        r.registerFunction(0x30fb60, putDrawEnv); // sceGsPutDrawEnv
        r.registerFunction(0x30f7b0, putDispEnv); // sceGsPutDispEnv
        // The whole picture is the port's: the window shows only what the port publishes (TTA's and Shinobido's set-up).
        auto &renderer = bozo::render::Renderer::instance();
        renderer.setEnabled(true);
        renderer.setPresentsAlone(true);
        r.vu0CodeMemory(vu0::codeMemory());
        // The game's VU0 microprograms (its only VCALLMS entries, pairs 0x156 and 0x1ba: the microcode VIF0 loads from
        // 0x3ae240 at boot) run in runtime/amp_vu1.cpp's interpreter on that code. Without them registered, the
        // engine's built-in table answered: Twisted Metal: Black's VU0 programs, whose results were wrong here (the
        // FreQs' arms and legs were not drawn).
        r.vu0Program(0x156 * 8, vu0Program<0x156>);
        r.vu0Program(0x1ba * 8, vu0Program<0x1ba>);
    }

    void reportRender()
    {
        std::printf("[amp:dma] transfers by channel:");
        for (int c = 0; c < 10; ++c)
            if (g_transfers[c])
                std::printf(" ch%d=%llu", c, (unsigned long long)g_transfers[c]);
        std::printf("; ring %llu tags, %llu bytes\n", (unsigned long long)g_ringTags, (unsigned long long)g_ringBytes);
        gs::report();
        vif::report();
        reportIpu();
    }
} // namespace amp

namespace amp::path3
{
    // VIF1's MSKPATH3 lifted: the GIF chains run at once when started (runtime/amp_render.cpp), nothing is held back.
    void unmasked() {}
}
