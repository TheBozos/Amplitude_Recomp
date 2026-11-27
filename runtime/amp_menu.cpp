// SPDX-License-Identifier: MIT
// The game's own menus lead to the port's settings: a PC SETTINGS entry in the Options menu and the pause menu.
//
// The entry itself is the game's (runtime/amp_menu_files.cpp: a button added to the two panels' definitions in
// meta.ui and to their scenes, read through the archive overlay). The game has nothing for it to do: selecting it
// opens the window's settings on the Amplitude tab (video, cheats, save states, quit), which holds the gamepads while
// open.
//
// Which button has the focus: a panel (the freq_panel class, vtable 0x3cfc28) keeps its focused component at +0x88,
// and a component its name (a symbol: a pointer to the string) at +4. Each panel drawn (0x19e0d0, the class's draw,
// once a frame for each panel shown) is looked at; a Cross pressed (scePadRead's data, any pad) while one showed
// pc_settings focused opens the settings.
#include "amp_functions.h"
#include "amp_options.h"
#include "amp_port.h"
#include "bozo/libs/guest.h"
#include "bozo/libs/libraries.h"
#include "bozo/port.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace amp
{
    namespace
    {
        constexpr uint32_t kFocus = 0x88u, kName = 0x4u, kCross = 0x4000u;

        uint64_t g_focusedAt = ~0ull;       // the vblank a panel last drew with PC SETTINGS focused
        std::map<uint32_t, bool> g_crossHeld; // per pad (port << 8 | slot): Cross down at its last read

        uint32_t word(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x1FFFFFFu), 4);
            return v;
        }

        bool inRam(uint32_t a) { return a >= 0x100000u && a < 0x1FFF000u; }

        std::string text(const uint8_t *ram, uint32_t a)
        {
            std::string s;
            if (!inRam(a))
                return s;
            for (uint32_t k = 0; k < 48; ++k)
            {
                const char c = static_cast<char>(ram[(a + k) & 0x1FFFFFFu]);
                if (!c)
                    break;
                s += c;
            }
            return s;
        }

        // 0x19e0d0(panel): a panel drawn. AMP_TRACE_MENU=1 prints each panel's focused component as it changes.
        void panelDraw(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t panel = ctx->r[4].uw[0];
            if (inRam(panel))
            {
                const uint32_t focus = word(ram, panel + kFocus);
                const std::string name = inRam(focus) ? text(ram, word(ram, focus + kName)) : std::string();
                if (name == "pc_settings")
                    g_focusedAt = bozo::port::vblankCount(rt);
                if (static const bool trace = std::getenv("AMP_TRACE_MENU") != nullptr; trace)
                {
                    static std::map<uint32_t, std::string> last;
                    if (last[panel] != name)
                    {
                        last[panel] = name;
                        std::printf("[amp:menu] panel %s (%x): focus %s\n", text(ram, word(ram, panel + 0xcu)).c_str(), panel,
                                    name.c_str());
                    }
                }
            }
            func_0x19e0d0(ram, ctx, rt);
        }

        // scePadRead(port, slot, data): the engine's answer; a Cross newly down while PC SETTINGS is focused opens the
        // settings.
        void padRead(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            bozo::libs::Libraries *l = libraries();
            const bozo::libs::Function *f = l ? l->find("scePadRead") : nullptr;
            bozo::libs::Guest g(*rt, ctx);
            const uint32_t port = g.arg(0), slot = g.arg(1), data = g.arg(2), ra = ctx->r[31].uw[0];
            if (f)
                f->handler(*l, g);
            ctx->pc = ra;
            if (ctx->r[2].sw[0] != 1)
                return;
            // (the buffer may be in an uncached mirror or the scratchpad: read as the game would)
            const uint32_t released = g.read8(data + 2) | (g.read8(data + 3) << 8);
            const bool cross = !(released & kCross);
            bool &held = g_crossHeld[(port << 8) | slot];
            const uint64_t now = bozo::port::vblankCount(rt);
            if (static const bool trace = std::getenv("AMP_TRACE_MENU") != nullptr; trace && cross && !held)
                std::printf("[amp:menu] Cross on pad %u/%u at vblank %llu (PC SETTINGS focused at %llu)\n", port, slot,
                            static_cast<unsigned long long>(now), static_cast<unsigned long long>(g_focusedAt));
            if (cross && !held && g_focusedAt != ~0ull && now - g_focusedAt <= 2)
            {
                std::printf("[amp:menu] PC SETTINGS: the settings opened\n");
                options::openSettings();
            }
            held = cross;
        }
    } // namespace

    void installMenu(bozo::port::Registry &r)
    {
        r.registerFunction(0x19e0d0, panelDraw); // a panel's draw (freq_panel)
        r.registerFunction(0x312230, padRead);   // scePadRead
    }
} // namespace amp
