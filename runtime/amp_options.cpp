// The player's options for Amplitude: graphics, cheats, quitting (runtime/amp_options.h).
#include "amp_options.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace amp::options
{
    namespace
    {
        std::atomic<bool> g_widescreen{false};
        std::atomic<int> g_imageScale{1};
        std::atomic<int> g_audioOffsetMs{50};
        std::array<std::atomic<bool>, static_cast<size_t>(Cheat::Count)> g_cheats{};
        // What a cheat set in the game's memory, to put back when it is turned off (the address written, 0: nothing).
        std::array<uint32_t, static_cast<size_t>(Cheat::Count)> g_applied{};
        std::mutex g_quitMutex;
        std::function<void()> g_quit;

        bool envOn(const char *name)
        {
            const char *v = std::getenv(name);
            return v && *v && std::strcmp(v, "0") != 0;
        }

        std::filesystem::path g_folder; // the user folder (load)
        std::filesystem::path iniPath() { return g_folder.empty() ? std::filesystem::path() : g_folder / "amplitude.ini"; }

        uint32_t word(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x1FFFFFFu), 4);
            return v;
        }
        bool inRam(uint32_t a) { return a >= 0x100000u && a < 0x1FFF000u; }

        // A game flag word set to 1 while the cheat is on, put back to 0 once when it is turned off.
        void flag(uint8_t *ram, Cheat c, uint32_t pointer, uint32_t field)
        {
            const size_t i = static_cast<size_t>(c);
            const uint32_t object = word(ram, pointer);
            if (cheat(c))
            {
                if (!inRam(object))
                    return;
                const uint32_t one = 1;
                std::memcpy(ram + object + field, &one, 4);
                g_applied[i] = object + field;
            }
            else if (g_applied[i])
            {
                const uint32_t zero = 0;
                std::memcpy(ram + (g_applied[i] & 0x1FFFFFFu), &zero, 4);
                g_applied[i] = 0;
            }
        }
    } // namespace

    bool widescreen() { return g_widescreen.load(std::memory_order_relaxed); }
    void setWidescreen(bool on) { g_widescreen.store(on, std::memory_order_relaxed); }
    float clipScaleX() { return widescreen() ? 0.75f : 1.0f; }
    int imageScale() { return g_imageScale.load(std::memory_order_relaxed); }
    void setImageScale(int scale) { g_imageScale.store(scale < 1 ? 1 : scale > 4 ? 4 : scale, std::memory_order_relaxed); }

    int audioOffsetMs() { return g_audioOffsetMs.load(std::memory_order_relaxed); }
    void setAudioOffsetMs(int ms) { g_audioOffsetMs.store(ms < -100 ? -100 : ms > 300 ? 300 : ms, std::memory_order_relaxed); }

    bool cheat(Cheat c) { return g_cheats[static_cast<size_t>(c)].load(std::memory_order_relaxed); }
    void setCheat(Cheat c, bool on) { g_cheats[static_cast<size_t>(c)].store(on, std::memory_order_relaxed); }

    void applyCheats(uint8_t *ram)
    {
        // The energy meter: a float, 15 when full, Game Over at 0, at +0x4bc of the song's player object *0x3af838 (the
        // HUD's copy, 0x43b74c, follows it; found by memory searches during a song with no input). Kept full; nothing
        // to put back.
        if (cheat(Cheat::InfiniteEnergy))
        {
            const uint32_t player = word(ram, 0x3af838u);
            if (inRam(player))
            {
                float energy;
                std::memcpy(&energy, ram + player + 0x4bcu, 4);
                if (energy > 0.0f && energy < 15.0f)
                {
                    const float full = 15.0f;
                    std::memcpy(ram + player + 0x4bcu, &full, 4);
                }
            }
        }
        // The game's own unlock-all flag: +0x8c of the game data at *0x440d44, which the arena list 0x26dd40 checks
        // before the FreQ's progress (the game unlocks Neotropolis when Basic Concepts Training is beaten).
        flag(ram, Cheat::UnlockAll, 0x440d44u, 0x8cu);
        // The game's own autopilot cheat: its debug toggle 0x110390 flips +0x100 of the object at *0x435f14 (and prints
        // "CHEAT: autopilot mode"): the game plays the gems itself.
        flag(ram, Cheat::Autopilot, 0x435f14u, 0x100u);
    }

    void load(const std::filesystem::path &folder)
    {
        g_folder = folder;
        setWidescreen(envOn("AMP_WIDESCREEN"));
        if (const char *scale = std::getenv("AMP_IMAGE_SCALE"); scale && *scale)
            setImageScale(std::atoi(scale));
        if (const char *offset = std::getenv("AMP_AUDIO_OFFSET"); offset && *offset)
            setAudioOffsetMs(std::atoi(offset));
        setCheat(Cheat::Autopilot, envOn("AMP_AUTOPILOT"));
        setCheat(Cheat::UnlockAll, envOn("AMP_UNLOCK_ALL"));
        setCheat(Cheat::InfiniteEnergy, envOn("AMP_INFINITE_ENERGY"));
        const std::filesystem::path path = iniPath();
        if (path.empty())
            return;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
        {
            const size_t eq = line.find('=');
            if (eq == std::string::npos || line[0] == '#' || line[0] == ';')
                continue;
            const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
            if (key == "widescreen" && !std::getenv("AMP_WIDESCREEN"))
                setWidescreen(value == "1");
            else if (key == "image_scale" && !std::getenv("AMP_IMAGE_SCALE"))
                setImageScale(std::atoi(value.c_str()));
            else if (key == "audio_offset_ms" && !std::getenv("AMP_AUDIO_OFFSET"))
                setAudioOffsetMs(std::atoi(value.c_str()));
        }
    }

    void save()
    {
        const std::filesystem::path path = iniPath();
        if (path.empty())
            return;
        std::ofstream out(path, std::ios::trunc);
        out << "# Amplitude's options (the window's Amplitude tab, F2)\n";
        out << "widescreen=" << (widescreen() ? 1 : 0) << "\n";
        out << "image_scale=" << imageScale() << "\n";
        out << "audio_offset_ms=" << audioOffsetMs() << "\n";
    }

    void setQuit(std::function<void()> quit)
    {
        std::lock_guard lock(g_quitMutex);
        g_quit = std::move(quit);
    }

    void quit()
    {
        std::function<void()> q;
        {
            std::lock_guard lock(g_quitMutex);
            q = g_quit;
        }
        if (q)
            q();
    }
} // namespace amp::options
