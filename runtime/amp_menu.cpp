// SPDX-License-Identifier: MIT
// PC SETTINGS in the game's Options menu and pause menu, and its pages drawn by the game itself.
//
// The entry is the game's own (runtime/amp_menu_files.cpp: a button added to the two panels' definitions and scenes,
// read through the archive overlay). Selected, it turns the panel's own rows into the PC pages, their labels set with
// the game's own text function, so the game draws them with its font, its highlight and its cursor:
//   main          VIDEO, CHEATS, SAVE STATES, QUIT GAME (the pause menu's two more rows: BACK, blank)
//   VIDEO         WIDESCREEN, SHARPER EFFECTS, AUDIO OFFSET, BACK
//   CHEATS        AUTOPILOT, INFINITE ENERGY, UNLOCK ALL, BACK
//   SAVE STATES   SLOT, SAVE STATE, LOAD STATE, BACK
// Up and Down are the game's (its navigator moves the cursor over the rows). Cross acts on the row (opens a page,
// switches a value, saves, loads; QUIT GAME asks for a second Cross), Left and Right change a value, Triangle and
// Circle go back a page and, from the main page, put the panel's own labels back. While a page is up those buttons
// (and Start, Select, Square) are taken out of the game's pad data, so the game does nothing with them.
//
// What the game keeps, from its code and memory:
// - A panel (the freq_panel class, vtable 0x3cfc28) is drawn by 0x19e0d0 once a frame while shown. It keeps its
//   focused component at +0x88 and its components in a map at +0x78 (the tree's header: its root at +4; a node's left
//   and right at +8 and +0xc, its name at +0x10, its component at +0x14). A component keeps its name (a symbol: a
//   pointer to the string) at +4 and its Text at +0x38.
// - A Text's label is set by 0x242d38(text, string) (as the panel's load sets each from english_screens); its string
//   is at +0x124.
// The game's functions are called on a copy of the thread's context (BozoEngine kernel.md section 3b: guest code on an
// override's own context never leaves it), its stack 0x200 bytes below the thread's, the labels' strings in between.
#include "amp_functions.h"
#include "amp_options.h"
#include "amp_port.h"
#include "bozo/libs/guest.h"
#include "bozo/libs/libraries.h"
#include "bozo/port.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace amp
{
    namespace
    {
        constexpr uint32_t kFocus = 0x88u, kComponents = 0x78u, kName = 0x4u, kText = 0x38u, kLabel = 0x124u;
        // The pad's buttons (active low in scePadRead's data, bytes 2-3).
        constexpr uint32_t kSelect = 0x0001u, kStart = 0x0008u, kUp = 0x0010u, kRight = 0x0020u, kDown = 0x0040u,
                           kLeft = 0x0080u, kTriangle = 0x1000u, kCircle = 0x2000u, kCross = 0x4000u, kSquare = 0x8000u;
        constexpr uint32_t kTaken = kSelect | kStart | kRight | kLeft | kTriangle | kCircle | kCross | kSquare;

        uint32_t word(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x1FFFFFCu), 4);
            return v;
        }
        bool inRam(uint32_t a) { return a >= 0x100000u && a < 0x1FFF000u; }
        std::string text(const uint8_t *ram, uint32_t a)
        {
            std::string s;
            if (!inRam(a))
                return s;
            for (uint32_t k = 0; k < 64; ++k)
            {
                const char c = static_cast<char>(ram[(a + k) & 0x1FFFFFFu]);
                if (!c)
                    break;
                s += c;
            }
            return s;
        }

        // The game's functions called on a copy of the thread's context (the header's note).
        struct Call
        {
            uint8_t *ram;
            Runtime *rt;
            R5900Context copy;
            uint32_t strings;
            Call(uint8_t *memory, const R5900Context *ctx, Runtime *runtime) : ram(memory), rt(runtime), copy(*ctx)
            {
                const uint32_t frame = (ctx->r[29].uw[0] - 0x200u) & ~0xFu;
                copy.r[29].ud[0] = frame;
                copy.leaving = 0;
                strings = (frame + 0x20u) & 0x1FFFFFFu;
            }
            void setText(uint32_t textObject, const std::string &label)
            {
                const size_t n = std::min<size_t>(label.size(), 120u);
                std::memcpy(ram + strings, label.data(), n);
                ram[strings + n] = 0;
                copy.r[4].ud[0] = textObject;
                copy.r[5].ud[0] = strings;
                copy.pc = 0x242d38u;
                func_0x242d38(ram, &copy, rt);
            }
        };

        enum class Page
        {
            Off,
            Main,
            Video,
            Cheats,
            States,
        };

        struct Pages
        {
            Page page = Page::Off;
            uint32_t panel = 0;
            std::vector<uint32_t> components; // the panel's rows, in its navigator's order
            std::vector<std::string> originals; // their labels, put back on leaving
            bool quitAsked = false;
            bool restoreForSave = false; // the labels put back a frame for a state's save, the page shown again after
            uint64_t panelDrawn = 0;
        } g_pages;

        uint64_t g_focusedAt = ~0ull; // the vblank a panel last drew with PC SETTINGS focused
        uint32_t g_focusedPanel = 0;
        std::map<uint32_t, uint32_t> g_held;  // per pad (port << 8 | slot): the buttons down at its last read
        std::map<uint32_t, uint32_t> g_taken; // per pad: buttons the pages took, hidden from the game until released
        uint32_t g_helpText = 0;              // the Options help panel's Text (seen as the game sets PC SETTINGS' help)
        constexpr const char *kEntryHelp = "Video, cheats, save states and quit";
        // A string of the port's own for the game: 0x108000-0x108FFF, recompiled code never read as data.
        constexpr uint32_t kStrings = 0x108000u;

        std::string rowHelp(Page page, size_t row);

        // The rows of the two panels, as their navigators order them (meta.ui, and the PC SETTINGS row added).
        const std::vector<std::string> *rowsOf(const std::string &panel)
        {
            static const std::vector<std::string> options = {"game", "memcard", "credits", "pc_settings"};
            static const std::vector<std::string> pause = {"resume", "quit", "restart", "controller_config", "game_settings",
                                                           "pc_settings"};
            return panel == "o_main" ? &options : panel == "s_pause" ? &pause : nullptr;
        }

        // The panel's components by name: its map's tree walked from the root.
        void collect(const uint8_t *ram, uint32_t node, uint32_t header, std::map<std::string, uint32_t> &out, int depth)
        {
            if (!inRam(node) || node == header || depth > 32)
                return;
            const uint32_t component = word(ram, node + 0x14u);
            if (inRam(component))
                out[text(ram, word(ram, component + kName))] = component;
            collect(ram, word(ram, node + 0x8u), header, out, depth + 1);
            collect(ram, word(ram, node + 0xcu), header, out, depth + 1);
        }

        std::string onOff(bool on) { return on ? "ON" : "OFF"; }

        std::vector<std::string> labels(Page page, size_t rows)
        {
            std::vector<std::string> l;
            switch (page)
            {
            case Page::Main:
                l = {"VIDEO", "CHEATS", "SAVE STATES", g_pages.quitAsked ? "CROSS AGAIN TO QUIT" : "QUIT GAME"};
                break;
            case Page::Video:
                l = {"WIDESCREEN: " + onOff(options::widescreen()), "SHARPER EFFECTS: " + onOff(options::imageScale() > 1),
                     "AUDIO OFFSET: " + std::to_string(options::audioOffsetMs()) + " MS", "BACK"};
                break;
            case Page::Cheats:
                l = {"AUTOPILOT: " + onOff(options::cheat(options::Cheat::Autopilot)),
                     "INFINITE ENERGY: " + onOff(options::cheat(options::Cheat::InfiniteEnergy)),
                     "UNLOCK ALL: " + onOff(options::cheat(options::Cheat::UnlockAll)), "BACK"};
                break;
            case Page::States:
            {
                const int slot = options::stateSlot();
                std::string summary = options::stateSummary(slot);
                const bool empty = summary.empty() || summary.find("empty") != std::string::npos;
                l = {"SLOT " + std::to_string(slot) + (empty ? " (EMPTY)" : " (SAVED)"), "SAVE STATE",
                     empty ? "LOAD STATE (NONE)" : "LOAD STATE", "BACK"};
                break;
            }
            case Page::Off:
                break;
            }
            // The pause menu's two more rows: BACK on the main page, blank otherwise.
            while (l.size() < rows)
                l.push_back(page == Page::Main && l.size() == 4 ? "BACK" : "");
            l.resize(rows);
            return l;
        }

        size_t focusedRow(const uint8_t *ram)
        {
            const uint32_t focus = word(ram, g_pages.panel + kFocus);
            const auto it = std::find(g_pages.components.begin(), g_pages.components.end(), focus);
            return it == g_pages.components.end() ? 0 : static_cast<size_t>(it - g_pages.components.begin());
        }

        void show(Call &call, const uint8_t *ram)
        {
            const std::vector<std::string> l = g_pages.page == Page::Off ? g_pages.originals
                                                                          : labels(g_pages.page, g_pages.components.size());
            for (size_t i = 0; i < g_pages.components.size(); ++i)
            {
                const uint32_t t = word(ram, g_pages.components[i] + kText);
                if (inRam(t) && text(ram, word(ram, t + kLabel)) != l[i])
                    call.setText(t, l[i]);
            }
            // The Options panel's help line: the row's own (the entry's again once the pages are left).
            if (inRam(g_helpText) && rowsOf("o_main") && g_pages.components.size() == 4)
            {
                const std::string h = g_pages.page == Page::Off ? std::string(kEntryHelp) : rowHelp(g_pages.page, focusedRow(ram));
                if (text(ram, word(ram, g_helpText + kLabel)) != h)
                    call.setText(g_helpText, h);
            }
        }

        std::string rowHelp(Page page, size_t row)
        {
            static const char *const help[][4] = {
                {"Widescreen, sharper effects, audio offset", "Autopilot, infinite energy, unlock all",
                 "Save or load the game at any point", "Close the game: press X twice"},
                {"16:9: the 3D wider, the menus stay 4:3", "Effects drawn at twice their size",
                 "Left / Right: how late the sound reaches you", "Back to PC settings"},
                {"The game plays the gems for you", "The energy meter stays full", "Every arena and song open",
                 "Back to PC settings"},
                {"Left / Right: choose the slot", "Save the game to this slot", "Load the game from this slot",
                 "Back to PC settings"},
            };
            const int page_ = page == Page::Main ? 0 : page == Page::Video ? 1 : page == Page::Cheats ? 2 : 3;
            return row < 4 ? help[page_][row] : std::string("Back to the menu");
        }

        bool enter(Call &call, const uint8_t *ram, uint32_t panel)
        {
            const std::vector<std::string> *rows = rowsOf(text(ram, word(ram, panel + 0xcu)));
            const uint32_t header = word(ram, panel + kComponents);
            if (!rows || !inRam(header))
                return false;
            std::map<std::string, uint32_t> byName;
            collect(ram, word(ram, header + 0x4u), header, byName, 0);
            Pages p;
            p.page = Page::Main;
            p.panel = panel;
            for (const std::string &name : *rows)
            {
                const auto it = byName.find(name);
                if (it == byName.end() || !inRam(word(ram, it->second + kText)))
                    return false;
                p.components.push_back(it->second);
                p.originals.push_back(text(ram, word(ram, word(ram, it->second + kText) + kLabel)));
            }
            p.panelDrawn = g_pages.panelDrawn;
            g_pages = std::move(p);
            show(call, ram);
            return true;
        }

        void leave(Call &call, const uint8_t *ram)
        {
            g_pages.page = Page::Off;
            show(call, ram);
            g_pages = Pages{};
        }

        // Cross, Left or Right (-1, +1) on row `row` of the page.
        void act(Call &call, const uint8_t *ram, size_t row, int step)
        {
            const bool cross = step == 0;
            Pages &p = g_pages;
            const bool back = row == 3 && p.page != Page::Main;
            if (row != 3 || p.page != Page::Main)
                p.quitAsked = false;
            switch (p.page)
            {
            case Page::Main:
                if (!cross)
                    return;
                if (row == 0)
                    p.page = Page::Video;
                else if (row == 1)
                    p.page = Page::Cheats;
                else if (row == 2)
                    p.page = Page::States;
                else if (row == 3)
                {
                    if (p.quitAsked)
                    {
                        std::printf("[amp:menu] QUIT GAME\n");
                        options::quit();
                    }
                    p.quitAsked = true;
                }
                else if (row == 4)
                {
                    leave(call, ram);
                    return;
                }
                break;
            case Page::Video:
                if (row == 0)
                    options::applyWidescreen(!options::widescreen());
                else if (row == 1)
                {
                    options::setImageScale(options::imageScale() > 1 ? 1 : 2);
                    options::save();
                }
                else if (row == 2 && !cross)
                {
                    options::setAudioOffsetMs(std::clamp(options::audioOffsetMs() + step * 10, 0, 200));
                    options::save();
                }
                break;
            case Page::Cheats:
                if (row <= 2)
                {
                    const options::Cheat c = row == 0 ? options::Cheat::Autopilot
                                                      : row == 1 ? options::Cheat::InfiniteEnergy : options::Cheat::UnlockAll;
                    options::setCheat(c, !options::cheat(c));
                }
                break;
            case Page::States:
                if (row == 0)
                    options::setStateSlot(options::stateSlot() + (cross ? 1 : step));
                else if (row == 1 && cross && options::hasStates())
                {
                    // The panel's own labels in the state, the page shown again after the save.
                    p.restoreForSave = true;
                    const Page page = p.page;
                    p.page = Page::Off;
                    show(call, ram);
                    p.page = page;
                    options::saveState(options::stateSlot());
                    return;
                }
                else if (row == 2 && cross && options::hasStates())
                {
                    options::loadState(options::stateSlot());
                    g_pages = Pages{}; // the state's own panel from its next frame
                    return;
                }
                break;
            case Page::Off:
                return;
            }
            if (back && cross)
                p.page = Page::Main;
            show(call, ram);
        }

        // 0x19e0d0(panel): a panel drawn. Its focus noted (AMP_TRACE_MENU=1 prints it as it changes).
        void panelDraw(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t panel = ctx->r[4].uw[0];
            if (inRam(panel))
            {
                const uint32_t focus = word(ram, panel + kFocus);
                const std::string name = inRam(focus) ? text(ram, word(ram, focus + kName)) : std::string();
                const uint64_t now = bozo::port::vblankCount(rt);
                if (name == "pc_settings")
                {
                    g_focusedAt = now;
                    g_focusedPanel = panel;
                }
                if (panel == g_pages.panel)
                    g_pages.panelDrawn = now;
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

        // scePadRead(port, slot, data): the engine's answer, then the PC pages' buttons (taken out of the game's data
        // while a page is up).
        void padRead(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            bozo::libs::Libraries *l = libraries();
            const bozo::libs::Function *f = l ? l->find("scePadRead") : nullptr;
            bozo::libs::Guest g(*rt, ctx);
            const uint32_t port = g.arg(0), slot = g.arg(1), data = g.arg(2), ra = ctx->r[31].uw[0];
            const R5900Context before = *ctx;
            if (f)
                f->handler(*l, g);
            ctx->pc = ra;
            if (ctx->r[2].sw[0] != 1)
                return;
            const uint32_t released = g.read8(data + 2) | (g.read8(data + 3) << 8);
            const uint32_t down = ~released & 0xFFFFu;
            const uint32_t pad = (port << 8) | slot;
            uint32_t &held = g_held[pad];
            const uint32_t pressed = down & ~held;
            held = down;
            uint32_t &taken = g_taken[pad];
            taken &= down; // a button the pages took stays the pages' until it is released
            const auto hide = [&](uint32_t buttons) {
                const uint32_t keep = released | buttons;
                const uint8_t bytes[2] = {static_cast<uint8_t>(keep), static_cast<uint8_t>(keep >> 8)};
                g.write(data + 2, bytes, 2);
            };
            const uint64_t now = bozo::port::vblankCount(rt);
            Call call(ram, &before, rt);
            Pages &p = g_pages;

            if (p.page == Page::Off)
            {
                if ((pressed & kCross) && g_focusedAt != ~0ull && now - g_focusedAt <= 2 && enter(call, ram, g_focusedPanel))
                {
                    std::printf("[amp:menu] PC SETTINGS: its pages\n");
                    taken |= kCross;
                }
                if (taken)
                    hide(taken);
                return;
            }
            // The panel gone (its screen left another way): the pages forgotten.
            if (now - p.panelDrawn > 30)
            {
                g_pages = Pages{};
                return;
            }
            if (p.restoreForSave)
            {
                p.restoreForSave = false;
                show(call, ram);
            }
            const size_t row = focusedRow(ram);
            taken |= down & kTaken;
            if (pressed & (kTriangle | kCircle))
            {
                if (p.page == Page::Main)
                    leave(call, ram);
                else
                {
                    p.page = Page::Main;
                    p.quitAsked = false;
                    show(call, ram);
                }
            }
            else if (pressed & kCross)
                act(call, ram, row, 0);
            else if (pressed & kLeft)
                act(call, ram, row, -1);
            else if (pressed & kRight)
                act(call, ram, row, +1);
            else if ((pressed & (kUp | kDown)) && p.quitAsked)
            {
                p.quitAsked = false;
                show(call, ram);
            }
            // The game sees none of the pages' buttons (Up and Down stay its: they move its cursor).
            hide(kTaken);
        }

        // 0x242d38(text, string): a Text's label set. The Options help panel's Text is the one the game gives PC
        // SETTINGS' help; while the pages are up, what the game sets there (a row's own help, as its cursor moves) is
        // the page row's help instead.
        void setText(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t t = ctx->r[4].uw[0];
            const std::string s = text(ram, ctx->r[5].uw[0]);
            if (s == kEntryHelp)
                g_helpText = t;
            if (t == g_helpText && g_pages.page != Page::Off && g_pages.components.size() == 4)
            {
                // The game's cursor has just moved: the row it is on now.
                const std::string h = rowHelp(g_pages.page, focusedRow(ram));
                std::memcpy(ram + kStrings, h.c_str(), h.size() + 1);
                ctx->r[5].ud[0] = kStrings;
            }
            func_0x242d38(ram, ctx, rt);
        }
    } // namespace

    void installMenu(bozo::port::Registry &r)
    {
        r.registerFunction(0x19e0d0, panelDraw); // a panel's draw (freq_panel)
        r.registerFunction(0x312230, padRead);   // scePadRead
        r.registerFunction(0x242d38, setText);   // a Text's label set (the Options help line)
    }
} // namespace amp
