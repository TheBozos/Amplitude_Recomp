// SPDX-License-Identifier: MIT
// PC SETTINGS in the game's Options menu and pause menu, and its pages drawn by the game itself.
//
// The entry is the game's own (runtime/amp_menu_files.cpp: a button added to the two panels' definitions and scenes,
// read through the archive overlay). Selected, it turns the panel's own rows into the PC pages, their labels set with
// the game's own text function, so the game draws them with its font, its highlight and its cursor:
//   main          VIDEO, SOUND, CHEATS, SAVE STATES, QUIT GAME, BACK
//   VIDEO         DISPLAY, RESOLUTION, RENDER SCALE, FRAME RATE, ANTIALIASING, TEXTURE SMOOTHING, TEXTURE UPSCALING,
//                 UPSCALING FILTER (the window's settings, applied and saved at once; DISPLAY and RESOLUTION by
//                 APPLY DISPLAY, after them), WIDESCREEN, SHARPER
//                 EFFECTS (the port's own), BACKGROUND PAUSE, BACK
//   SOUND         AUDIO OFFSET, BACK
//   CHEATS        AUTOPILOT, INFINITE ENERGY, UNLOCK ALL, BACK
//   SAVE STATES   SLOT, SAVE STATE, LOAD STATE, BACK
// A page longer than the panel's rows (4 in Options, 6 in the pause menu) scrolls: Down on the last row shows the next
// item instead of the game's cursor wrapping, Up on the first the one before; the cursor stops at the list's ends.
// Up and Down are otherwise the game's (its navigator moves the cursor). Cross acts on the item (opens a page, switches
// a value, saves, loads; QUIT GAME asks for a second Cross), Left and Right change a value, Triangle and Circle go back
// a page and, from the main page, put the panel's own labels back. While a page is up those buttons (and Start,
// Select, Square, and Up / Down when they scroll) are taken out of the game's pad data, so the game does nothing with
// them. A page shorter than the panel brings the cursor up onto its last item with presses of Up the port gives the
// game.
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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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
        constexpr const char *kEntryHelp = "Video, cheats, save states and quit";
        // A string of the port's own for the game: 0x108000-0x108FFF, recompiled code never read as data.
        constexpr uint32_t kStrings = 0x108000u;

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

        // ---- The pages ---------------------------------------------------------------------------------------------

        enum class Page
        {
            Off,
            Main,
            Video,
            Sound,
            Cheats,
            States,
        };

        struct Item
        {
            std::function<std::string()> label;
            std::function<void(int)> act; // 0: Cross; -1 / +1: Left / Right
            std::string help;
        };

        struct Pages
        {
            Page page = Page::Off;
            uint32_t panel = 0;
            std::vector<uint32_t> components;   // the panel's rows, in its navigator's order
            std::vector<std::string> originals; // their labels, put back on leaving
            size_t offset = 0;                  // the page's first item shown
            bool quitAsked = false;
            bool savePending = false;    // SAVE STATE chosen: the panel's own labels up, then the save asked for
            uint64_t restoreAt = 0;      // until this vblank the panel's own labels stay up (the save taken meanwhile)
            uint64_t panelDrawn = 0;
            options::Video video;        // VIDEO's edits of the window's settings, applied by APPLY AND SAVE
            int moves = 0;               // cursor moves still to give the game: Down presses (+) or Up presses (-)
            uint64_t moveAt = 0;         // the vblank of the last half given (one press, one release, a frame each)
            uint64_t heldSince = 0, repeatAt = 0, navAt = 0; // Up / Down held: their repeat
        } g_pages;

        uint64_t g_focusedAt = ~0ull; // the vblank a panel last drew with PC SETTINGS focused
        uint32_t g_focusedPanel = 0;
        std::map<uint32_t, uint32_t> g_held;  // per pad (port << 8 | slot): the buttons down at its last read
        std::map<uint32_t, uint32_t> g_taken; // per pad: buttons the pages took, hidden from the game until released
        uint32_t g_helpText = 0;              // the Options help panel's Text (seen as the game sets PC SETTINGS' help)
        uint64_t g_now = 0;                   // this read's vblank

        std::string onOff(bool on) { return on ? "ON" : "OFF"; }
        std::string times(int n) { return std::to_string(n) + "X"; }
        template <typename T> T step(const std::vector<T> &list, const T &value, int delta)
        {
            const int n = static_cast<int>(list.size());
            int i = 0;
            for (int k = 0; k < n; ++k)
                if (list[static_cast<size_t>(k)] == value)
                    i = k;
            return list[static_cast<size_t>(((i + (delta ? delta : 1)) % n + n) % n)];
        }

        void open(Page page);

        std::vector<Item> items(Page page)
        {
            Pages &p = g_pages;
            const Item back{[] { return std::string("BACK"); }, [](int d) { if (!d) open(Page::Main); }, "Back to PC settings"};
            std::vector<Item> l;
            switch (page)
            {
            case Page::Main:
                l = {{[] { return std::string("VIDEO"); }, [](int d) { if (!d) open(Page::Video); },
                      "Display, quality, widescreen"},
                     {[] { return std::string("SOUND"); }, [](int d) { if (!d) open(Page::Sound); }, "The audio offset"},
                     {[] { return std::string("CHEATS"); }, [](int d) { if (!d) open(Page::Cheats); },
                      "Autopilot, energy, unlocks"},
                     {[] { return std::string("SAVE STATES"); }, [](int d) { if (!d) open(Page::States); },
                      "Save or load at any point"},
                     {[&p] { return std::string(p.quitAsked ? "X AGAIN TO QUIT" : "QUIT GAME"); },
                      [&p](int d) {
                          if (d)
                              return;
                          if (p.quitAsked)
                          {
                              std::printf("[amp:menu] QUIT GAME\n");
                              options::quit();
                          }
                          p.quitAsked = true;
                      },
                      "Close the game: press X twice"},
                     {[] { return std::string("BACK"); }, [](int d) { if (!d) open(Page::Off); }, "Back to the menu"}};
                break;
            case Page::Video:
            {
                const options::VideoChoices c = options::videoChoices();
                options::Video &v = p.video;
                if (options::videoAvailable())
                {
                    const std::vector<int> modes = {0, 1, 2};
                    const std::vector<float> scales = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
                    const std::vector<int> aa = {0, 2, 4, 8}, smoothing = {0, 1, 2, 4, 8, 16}, upscales = {1, 2, 4};
                    std::vector<int> rates = {0, -1};
                    if (v.frameRate > 0)
                        rates.push_back(v.frameRate);
                    const auto resolutions = c.resolutions;
                    const int refresh = c.monitorRefresh;
                    // A setting changed here is applied and saved at once, the display and resolution as they are applied
                    // (those two wait for APPLY DISPLAY: they change the window).
                    const auto live = [](const options::Video &edit) {
                        options::Video now = edit;
                        const options::Video applied = options::videoChoices().applied;
                        now.mode = applied.mode;
                        now.width = applied.width;
                        now.height = applied.height;
                        options::postVideo(now, true);
                    };
                    l.push_back({[&v] { return std::string("DISPLAY: ") + (v.mode == 1 ? "FULLSCREEN" : v.mode == 2 ? "BORDERLESS" : "WINDOWED"); },
                                 [&v, modes](int d) { v.mode = step(modes, v.mode, d); }, "Window, fullscreen, borderless"});
                    l.push_back({[&v] {
                                     return "RESOLUTION: " + (v.width > 0 && v.height > 0 ? std::to_string(v.width) + " X " + std::to_string(v.height)
                                                                                           : std::string("DEFAULT"));
                                 },
                                 [&v, resolutions](int d) {
                                     if (resolutions.empty())
                                         return;
                                     const auto next = step(resolutions, std::make_pair(v.width, v.height), d);
                                     v.width = next.first;
                                     v.height = next.second;
                                 },
                                 "The window's size"});
                    l.push_back({[&v] {
                                     const options::Video a = options::videoChoices().applied;
                                     return std::string(v.mode == a.mode && v.width == a.width && v.height == a.height ? "APPLY DISPLAY"
                                                                                                                      : "APPLY DISPLAY *");
                                 },
                                 [&v](int d) {
                                     if (d)
                                         return;
                                     std::printf("[amp:menu] VIDEO: display applied and saved\n");
                                     options::postVideo(v, true);
                                 },
                                 "The display and size above"});
                    l.push_back({[&v] { return "RENDER SCALE: " + std::to_string(static_cast<int>(std::lround(v.renderScale * 100.0f))) + "%"; },
                                 [&v, scales, live](int d) {
                                     float best = scales[2];
                                     for (float s : scales)
                                         if (std::fabs(s - v.renderScale) < 0.01f)
                                             best = s;
                                     v.renderScale = step(scales, best, d);
                                     live(v);
                                 },
                                 "How sharp the 3D is drawn"});
                    l.push_back({[&v, refresh] {
                                     return std::string("FRAME RATE: ") +
                                            (v.frameRate == 0 ? std::string("60 (GAME)")
                                             : v.frameRate < 0 ? (refresh > 0 ? "MONITOR (" + std::to_string(refresh) + ")" : std::string("MONITOR"))
                                                               : std::to_string(v.frameRate));
                                 },
                                 [&v, rates, live](int d) { v.frameRate = step(rates, v.frameRate, d), live(v); },
                                 "60, or the monitor's rate"});
                    l.push_back({[&v] { return "ANTIALIASING: " + (v.antiAliasing ? times(v.antiAliasing) : std::string("OFF")); },
                                 [&v, aa, live](int d) { v.antiAliasing = step(aa, v.antiAliasing, d), live(v); }, "Smoother edges (MSAA)"});
                    l.push_back({[&v] {
                                     return "TEXTURE SMOOTHING: " + (v.textureSmoothing == 0   ? std::string("OFF")
                                                                     : v.textureSmoothing == 1 ? std::string("TRILINEAR")
                                                                                               : times(v.textureSmoothing));
                                 },
                                 [&v, smoothing, live](int d) { v.textureSmoothing = step(smoothing, v.textureSmoothing, d), live(v); },
                                 "Off is the console's own look"});
                    l.push_back({[&v] { return "TEXTURE UPSCALING: " + (v.upscale > 1 ? times(v.upscale) : std::string("OFF")); },
                                 [&v, upscales, live](int d) { v.upscale = step(upscales, v.upscale, d), live(v); },
                                 "Textures from larger copies"});
                    l.push_back({[&v] { return std::string("UPSCALING FILTER: ") + (v.smoothUpscale ? "SMOOTH" : "EDGE"); },
                                 [&v, live](int) { v.smoothUpscale = !v.smoothUpscale, live(v); }, "How the larger copies are made"});
                }
                l.push_back({[] { return "WIDESCREEN: " + onOff(options::widescreen()); },
                             [](int) { options::applyWidescreen(!options::widescreen()); },
                             "16:9 3D, the menus 4:3"});
                l.push_back({[] { return "SHARPER EFFECTS: " + onOff(options::imageScale() > 1); },
                             [](int) {
                                 options::setImageScale(options::imageScale() > 1 ? 1 : 2);
                                 options::save();
                             },
                             "Effects at twice their size"});
                if (options::videoAvailable())
                {
                    l.push_back({[&v] { return "BACKGROUND PAUSE: " + onOff(v.pauseInBackground); },
                                 [&v](int) {
                                     v.pauseInBackground = !v.pauseInBackground;
                                     options::Video now = v;
                                     const options::Video applied = options::videoChoices().applied;
                                     now.mode = applied.mode;
                                     now.width = applied.width;
                                     now.height = applied.height;
                                     options::postVideo(now, true);
                                 },
                                 "Pause when not in focus"});
                }
                l.push_back(back);
                break;
            }
            case Page::Sound:
                l = {{[] { return "AUDIO OFFSET: " + std::to_string(options::audioOffsetMs()) + " MS"; },
                      [](int d) {
                          options::setAudioOffsetMs(std::clamp(options::audioOffsetMs() + (d ? d : 1) * 10, 0, 200));
                          options::save();
                      },
                      "Left / Right: the sound's delay"},
                     back};
                break;
            case Page::Cheats:
            {
                const auto cheat = [](options::Cheat c, const char *name, const char *help) {
                    return Item{[c, name] { return std::string(name) + ": " + onOff(options::cheat(c)); },
                                [c](int) { options::setCheat(c, !options::cheat(c)); }, help};
                };
                l = {cheat(options::Cheat::Autopilot, "AUTOPILOT", "The game plays the gems for you"),
                     cheat(options::Cheat::InfiniteEnergy, "INFINITE ENERGY", "The energy meter stays full"),
                     cheat(options::Cheat::UnlockAll, "UNLOCK ALL", "Every arena and song open"), back};
                break;
            }
            case Page::States:
            {
                const auto empty = [] {
                    const std::string summary = options::stateSummary(options::stateSlot());
                    return summary.empty() || summary.find("empty") != std::string::npos;
                };
                l = {{[empty] { return "SLOT " + std::to_string(options::stateSlot()) + (empty() ? " (EMPTY)" : " (SAVED)"); },
                      [](int d) { options::setStateSlot(options::stateSlot() + (d ? d : 1)); }, "Left / Right: choose the slot"},
                     {[] { return std::string("SAVE STATE"); }, [&p](int d) {
                          if (d || !options::hasStates())
                              return;
                          p.savePending = true; // the panel's own labels in the state (settle, padRead)
                      },
                      "Save the game to this slot"},
                     {[empty] { return std::string(empty() ? "LOAD STATE (NONE)" : "LOAD STATE"); }, [](int d) {
                          if (d || !options::hasStates())
                              return;
                          options::loadState(options::stateSlot());
                          g_pages = Pages{}; // the state's own panel from its next frame
                      },
                      "Load the game from this slot"},
                     back};
                break;
            }
            case Page::Off:
                break;
            }
            return l;
        }

        // ---- The panel ---------------------------------------------------------------------------------------------

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

        size_t focusedRow(const uint8_t *ram)
        {
            const uint32_t focus = word(ram, g_pages.panel + kFocus);
            const auto it = std::find(g_pages.components.begin(), g_pages.components.end(), focus);
            return it == g_pages.components.end() ? 0 : static_cast<size_t>(it - g_pages.components.begin());
        }

        bool hasHelp() { return inRam(g_helpText) && g_pages.components.size() == 4; } // the Options panel's help line

        std::string currentHelp(const uint8_t *ram)
        {
            if (g_pages.page == Page::Off)
                return kEntryHelp;
            const std::vector<Item> l = items(g_pages.page);
            const size_t i = g_pages.offset + focusedRow(ram);
            return i < l.size() ? l[i].help : std::string();
        }

        void show(Call &call, const uint8_t *ram)
        {
            const Pages &p = g_pages;
            std::vector<std::string> labels = p.originals;
            if (p.page != Page::Off)
            {
                const std::vector<Item> l = items(p.page);
                for (size_t i = 0; i < labels.size(); ++i)
                    labels[i] = p.offset + i < l.size() ? l[p.offset + i].label() : std::string();
                // A page longer than the rows shows it scrolls: ^ before the top row with items above, v after the bottom
                // row with items below.
                if (p.offset > 0 && !labels.empty())
                    labels.front() = "^ " + labels.front();
                if (p.offset + labels.size() < l.size() && !labels.empty())
                    labels.back() += " v";
            }
            for (size_t i = 0; i < p.components.size(); ++i)
            {
                const uint32_t t = word(ram, p.components[i] + kText);
                if (inRam(t) && text(ram, word(ram, t + kLabel)) != labels[i])
                    call.setText(t, labels[i]);
            }
            if (hasHelp())
            {
                const std::string h = currentHelp(ram);
                if (text(ram, word(ram, g_helpText + kLabel)) != h)
                    call.setText(g_helpText, h);
            }
        }

        // A page opened (Off: the panel's own labels back): its first item at the top, the cursor on its row, or brought
        // up onto the page's last item when the page is shorter.
        Page g_opened = Page::Off;
        void open(Page page) { g_opened = page == Page::Off ? Page::Main : page, g_pages.page = page == Page::Off ? Page::Off : page; }

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
            // The cursor is on PC SETTINGS, the last row: brought up to the first item.
            g_pages.moves = -static_cast<int>(focusedRow(ram));
            show(call, ram);
            return true;
        }

        // After an item's action: a page opened (or left) shown from its first item; the cursor kept on a real item.
        void settle(Call &call, const uint8_t *ram)
        {
            Pages &p = g_pages;
            if (p.savePending)
            {
                // The panel's own labels up until the save is taken (at a frame's end), then the page again (padRead).
                p.savePending = false;
                const Page page = p.page;
                p.page = Page::Off;
                show(call, ram);
                p.page = page;
                p.restoreAt = g_now + 3;
                options::saveState(options::stateSlot());
                return;
            }
            if (g_opened != Page::Off || p.page == Page::Off)
            {
                g_opened = Page::Off;
                p.quitAsked = false;
                p.offset = 0;
                if (p.page == Page::Video)
                {
                    const options::VideoChoices c = options::videoChoices();
                    p.video = c.applied;
                }
                if (p.page == Page::Off)
                {
                    show(call, ram);
                    g_pages = Pages{};
                    return;
                }
                // The page from its first item, the cursor brought up to it.
                p.moves = -static_cast<int>(focusedRow(ram));
            }
            show(call, ram);
        }

        // ---- The game's functions ----------------------------------------------------------------------------------

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
            // The pad's mode (0x41 digital, 0x73 analog: the sticks at +4..+7, 0x79 with the buttons' pressures at
            // +8..+19, which the game's cursor goes by too). While the pages are up the left stick is Up / Down.
            const uint8_t mode = g.read8(data + 1);
            const bool sticks = mode == 0x73u || mode == 0x79u;
            uint32_t stick = 0;
            if (sticks && g_pages.page != Page::Off)
            {
                const uint8_t ly = g.read8(data + 7);
                stick = ly < 0x40u ? kUp : ly > 0xC0u ? kDown : 0u;
            }
            const uint32_t down = (~released & 0xFFFFu) | stick;
            const uint32_t pad = (port << 8) | slot;
            uint32_t &held = g_held[pad];
            const uint32_t pressed = down & ~held;
            held = down;
            uint32_t &taken = g_taken[pad];
            taken &= down; // a button the pages took stays the pages' until it is released
            uint32_t give = 0; // buttons the port presses for the game (a short page's cursor brought up)
            const auto write = [&](uint32_t hidden) {
                const uint32_t keep = (released | hidden) & ~give;
                const uint8_t bytes[2] = {static_cast<uint8_t>(keep), static_cast<uint8_t>(keep >> 8)};
                g.write(data + 2, bytes, 2);
                // The hidden buttons' pressures too (the pressure bytes: Right, Left, Up, Down, Triangle, Circle, Cross,
                // Square, from +8), and the left stick centred while the pages are up.
                if (mode == 0x79u)
                {
                    static const uint32_t order[8] = {kRight, kLeft, kUp, kDown, kTriangle, kCircle, kCross, kSquare};
                    for (uint32_t k = 0; k < 8; ++k)
                        if (hidden & order[k] & ~give)
                        {
                            const uint8_t zero = 0;
                            g.write(data + 8 + k, &zero, 1);
                        }
                }
                if (sticks && g_pages.page != Page::Off)
                {
                    const uint8_t centre[2] = {0x80, 0x80};
                    g.write(data + 6, centre, 2);
                }
            };
            const uint64_t now = bozo::port::vblankCount(rt);
            g_now = now;
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
                    write(taken);
                return;
            }
            // The panel gone (its screen left another way): the pages forgotten.
            if (now - p.panelDrawn > 30)
            {
                g_pages = Pages{};
                return;
            }
            if (p.restoreAt)
            {
                // A save under way: the panel's own labels stay up, nothing reaches the game; then the page again.
                if (now < p.restoreAt)
                {
                    write(kTaken | kUp | kDown);
                    return;
                }
                p.restoreAt = 0;
            }
            // The cursor moves given to the game, a frame pressed and a frame released each.
            if (p.moves != 0 && now != p.moveAt)
            {
                const bool press = (p.moveAt & 1u) == 0;
                p.moveAt = now | (press ? 1u : 0u); // (its low bit: a press given, its release next)
                p.moveAt = press ? (now | 1u) : (now & ~1ull);
                if (press)
                    give = p.moves > 0 ? kDown : kUp;
                else
                    p.moves += p.moves > 0 ? -1 : 1;
            }
            show(call, ram); // (a label refreshed the frame after a save, or as a value changed elsewhere)
            const std::vector<Item> list = items(p.page);
            const size_t rows = p.components.size();
            // The row the cursor is on once the moves given are taken.
            const int virtualRow = std::clamp(static_cast<int>(focusedRow(ram)) + p.moves, 0, static_cast<int>(rows) - 1);
            const size_t row = static_cast<size_t>(virtualRow), index = p.offset + row;
            if (static const bool trace = std::getenv("AMP_TRACE_MENU") != nullptr; trace && pressed)
                std::printf("[amp:menu] vblank %llu pad %x: pressed %04x, row %zu, offset %zu, item %zu of %zu\n",
                            static_cast<unsigned long long>(now), pad, pressed, row, p.offset, index, list.size());
            taken |= down & kTaken;
            // Up / Down: the port's (a press, then a repeat while held, once a vblank), the game's cursor moved by the
            // presses the port gives it, the list scrolled at the panel's edges; the cursor stops at the list's ends.
            int nav = 0;
            if (pressed & (kUp | kDown))
            {
                nav = (pressed & kDown) ? 1 : -1;
                p.heldSince = p.repeatAt = now;
            }
            else if ((down & (kUp | kDown)) && now != p.navAt && now - p.heldSince >= 18 && now - p.repeatAt >= 5)
            {
                nav = (down & kDown) ? 1 : -1;
                p.repeatAt = now;
            }
            if (nav)
                p.navAt = now;
            if (p.moves != 0 && nav)
                nav = 0; // (one move at a time: the next once the game's cursor has taken this one)
            if (pressed & (kTriangle | kCircle))
            {
                open(p.page == Page::Main ? Page::Off : Page::Main);
                settle(call, ram);
            }
            else if ((pressed & kCross) && index < list.size())
            {
                list[index].act(0);
                settle(call, ram);
            }
            else if ((pressed & (kLeft | kRight)) && index < list.size())
            {
                list[index].act((pressed & kLeft) ? -1 : 1);
                settle(call, ram);
            }
            else if (nav > 0 && index + 1 < list.size())
            {
                if (row + 1 >= rows)
                    ++p.offset;
                else
                    p.moves = 1;
                p.quitAsked = false;
                show(call, ram);
            }
            else if (nav < 0)
            {
                if (row > 0)
                    p.moves = -1;
                else if (p.offset > 0)
                    --p.offset;
                p.quitAsked = false;
                show(call, ram);
            }
            // The game sees none of the pages' buttons, Up and Down but the presses given.
            write(kTaken | kUp | kDown | taken);
        }

        // 0x242d38(text, string): a Text's label set. The Options help panel's Text is the one the game gives PC
        // SETTINGS' help; while the pages are up, what the game sets there (a row's own help, as its cursor moves) is
        // the page item's help instead.
        void setText(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t t = ctx->r[4].uw[0];
            const std::string s = text(ram, ctx->r[5].uw[0]);
            if (s == kEntryHelp)
                g_helpText = t;
            if (t == g_helpText && g_pages.page != Page::Off && hasHelp())
            {
                const std::string h = currentHelp(ram);
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
