// Amplitude on BozoEngine: the headless runner (amp-headless) and the window (amplitude).
// The headless runner prints what is not native yet: the end reason, unknown device accesses by site, library calls by
// name (AMP_CALL_STATS=1). Each stop is the next thing to name or answer.
#include "bozo/app/frame_pacer.h"
#include "bozo/audio/wav.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/host/free_host.h"
#include "bozo/port.h"
#include "amp_functions.h"
#include "amp_port.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>
#ifdef AMP_WITH_WINDOW
#include "bozo/app/app.h"
#include "bozo/app/audio_device.h"
#include "bozo/app/device_pad.h"
#include "bozo/app/window.h"
#endif

int main(int argc, char **argv)
{
    bozo::host::FreeOptions options;
    options.framePeriodCycles = 294912000 / 60; // NTSC (VMODE = NTSC in SYSTEM.CNF)
    bool window = false;
    bool mute = false; // --mute: no sound through the speakers (hidden runs, tests, captures)
#ifdef AMP_WITH_WINDOW
    window = true;
    bozo::app::setIdentity({"Amplitude", "Amplitude_Recomp", "amplitude-recomp", "SCUS_972.58"});
    bozo::app::WindowOptions windowOptions;
    windowOptions.title = "Amplitude - BozoEngine";
    windowOptions.width = 960;
    windowOptions.height = 720;
    // AMP_CAPTURE_FRAMES=<item>[,<item>...], each <frame> or <first>:<last>:<step>: the window's picture at those game
    // frames, as frame_<n>.png in PS2X_CAPTURE_WINDOW_DIR (play tests and README screenshots: what the screen shows).
    if (const char *e = std::getenv("AMP_CAPTURE_FRAMES"))
        for (const char *p = e; p && *p;)
        {
            unsigned long long first, last, step;
            if (std::sscanf(p, "%llu:%llu:%llu", &first, &last, &step) == 3 && step > 0)
                for (unsigned long long f = first; f <= last; f += step)
                    windowOptions.captureFrames.push_back(f);
            else if (std::sscanf(p, "%llu", &first) == 1)
                windowOptions.captureFrames.push_back(first);
            p = std::strchr(p, ',');
            if (p)
                ++p;
        }
#endif
    options.elf = AMP_DEFAULT_ELF;
    options.iso = AMP_DEFAULT_ISO;
    // SCUS_972.58 is stripped: the symbol map (recomp/amp_symbols.txt) names the functions the engine answers.
    options.symbolMap = AMP_SYMBOL_MAP;
    // The boot waits for the IOP sound modules' RPC servers before any frame ends: they advance while it waits.
    options.advanceLibrariesWhileStalled = true;
    options.frames = window ? 0 : 600;
    options.printGameOutput = true;
    std::string wav, dumpRam;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc)
            options.frames = std::stoul(argv[++i]);
        else if (a == "--pad-script" && i + 1 < argc)
            options.padScript = argv[++i];
        else if ((a == "--pad-script2" || a == "--pad-script3" || a == "--pad-script4") && i + 1 < argc)
            options.playerScripts[static_cast<size_t>(a.back() - '2')] = argv[++i]; // players 2-4 (3, 4: multitap)
        else if (a == "--card-dir" && i + 1 < argc)
            options.cardDir = argv[++i];
        else if (a == "--out" && i + 1 < argc)
            options.out = argv[++i];
        else if (a == "--elf" && i + 1 < argc)
            options.elf = argv[++i];
        else if (a == "--iso" && i + 1 < argc)
            options.iso = argv[++i];
        else if (a == "--symbols" && i + 1 < argc)
            options.symbolMap = argv[++i];
        else if (a == "--wav" && i + 1 < argc)
            wav = argv[++i];
        else if (a == "--headless")
            window = false;
        else if (a == "--profile-io")
            options.profileIo = true;
        else if (a == "--mute")
            mute = true;
        else if (a == "--quiet")
            options.printGameOutput = false;
        else if (a == "--dump-ram" && i + 1 < argc)
            dumpRam = argv[++i];
        else if (a == "--load-state" && i + 1 < argc)
            options.loadState = argv[++i];
        else if (a == "--save-state" && i + 1 < argc)
            options.saveState = argv[++i];
        else if (a == "--save-state-at" && i + 1 < argc)
            options.saveStateFrame = std::stoul(argv[++i]);
#ifdef AMP_WITH_WINDOW
        else if (a == "--no-vsync")
            windowOptions.vsync = false;
        else if (a == "--fullscreen")
            windowOptions.fullscreen = true;
        else if (a == "--hidden") // no visible window: it presents offscreen (screenshots and captures still work)
            windowOptions.hidden = true;
#endif
        else
        {
            std::fprintf(stderr, "Usage: amplitude [--frames N] [--headless] [--elf FILE] [--iso FILE] [--symbols FILE]\n"
                                 "                 [--card-dir DIR] [--pad-script FILE] [--pad-script2..4 FILE] [--wav FILE] [--out FILE]\n"
                                 "                 [--profile-io] [--quiet] [--mute]\n"
                                 "                 [--dump-ram FILE] [--load-state FILE] [--save-state FILE --save-state-at N]\n"
                                 "                 [--fullscreen] [--no-vsync] [--hidden]\n");
            return a == "--help" ? 0 : 2;
        }
    }
#ifdef AMP_WITH_WINDOW
    // The memory cards of a played game live in the user folder (%APPDATA%\Amplitude_Recomp), so the FreQs and the
    // progress (the arenas unlocked) outlive the session; without a folder they were fresh cards in memory every time.
    // Headless and scripted runs (a pad script, a hidden window's captures) keep fresh cards in memory unless given
    // --card-dir, so a test never touches the player's saves; AMP_MEMORY_CARDS=1 keeps them in memory for a played game
    // too. (Onimusha_Recomp's rule.)
    if (window && !windowOptions.hidden && options.padScript.empty() && options.cardDir.empty() &&
        !std::getenv("AMP_MEMORY_CARDS") && !bozo::app::userDirectory().empty())
    {
        options.cardDir = bozo::app::userDirectory().string();
        std::printf("[amp] memory cards in %s\n", options.cardDir.c_str());
    }
#endif
    options.realTime = window;
    options.gameTag = "SCUS-97258"; // a save state loads only into Amplitude
    bozo::host::FreeHost host(options);
    bozo::ee::Dispatcher d(host);
    amp::installAudio(host.libraries(), d.runtime().memory.ram());
    amp::installState(*host.stateSections());
    // The movies (libmpeg native): the SDK's own layout. The engine's default (Twisted Metal: Black's reference layout)
    // writes TMB's fixed addresses and takes 0x600 bytes from its heap. The stream callbacks' 32-byte entries still come
    // from that heap (the SDK demux, sdkDemux, stalls Amplitude's movie: its sound callback is not Shinobido's).
    host.libraries().mpegOptions.referenceLayout = false;
    // The engine's own heap (libmpeg's callback entries, any native allocation): 32 KB inside .text, which runs
    // recompiled and is never read as data. Its default, from the heap start to the end of RAM, is where the game's own
    // allocator works: both handed out the same memory (the task manager's list header at 0x549ea0 was overwritten).
    host.libraries().heap.reset(0x101000, 0x109000);
    d.setFunctions(amp_function_table, amp_function_count, amp_resume_table, amp_resume_count);
    bozo::port::Registry registry;
    amp::install(d.runtime(), registry);
    std::string error;
    if (!host.setUp(d, amp_external_table, amp_external_count, &registry, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }

    bozo::app::FramePacer pacer;
    // AMP_UNPACED=1: the window as fast as the PC goes (with --no-vsync): unattended captures reach the menus in
    // seconds instead of the intro's two minutes.
    pacer.setPaced(window && !std::getenv("AMP_UNPACED"));
    host.onFrame = [&](uint64_t f, const R5900Context *ctx)
    {
        // AMP_DUMP_RAM_AT=<frame>,<frame>...: the EE's RAM at each of those frames' end, as build/ram_<frame>.bin (memory
        // searches: the cheats' fields).
        if (static const char *at = std::getenv("AMP_DUMP_RAM_AT"); at)
            for (const char *p = at; *p;)
            {
                char *next = nullptr;
                if (std::strtoull(p, &next, 10) == f)
                    if (FILE *file = std::fopen(("build/ram_" + std::to_string(f) + ".bin").c_str(), "wb"))
                    {
                        std::fwrite(d.runtime().memory.ram(), 1, 0x2000000u, file);
                        std::fclose(file);
                    }
                if (!next || next == p)
                    break;
                p = *next ? next + 1 : next;
            }
        // AMP_INFINITE_ENERGY=1: the energy meter kept full while a song runs (a float, 15 when full, Game Over at 0, at
        // +0x4bc of the song's player object *0x3af838; the HUD's copy, 0x43b74c, follows it; found by memory searches
        // during a song with no input). For play tests: a whole song without input.
        if (static const bool infinite = std::getenv("AMP_INFINITE_ENERGY") != nullptr; infinite)
        {
            uint8_t *ram = d.runtime().memory.ram();
            uint32_t player;
            std::memcpy(&player, ram + 0x3af838u, 4);
            if (player >= 0x100000u && player < 0x1FFF000u)
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
        // AMP_UNLOCK_ALL=1: the game's own unlock-all flag (+0x8c of the game data at *0x440d44, which the arena list
        // 0x26dd40 checks before the FreQ's progress): every arena and song open without beating the training first
        // (the game unlocks Neotropolis when Basic Concepts Training is beaten). For play tests.
        if (static const bool unlockAll = std::getenv("AMP_UNLOCK_ALL") != nullptr; unlockAll)
        {
            uint32_t data;
            std::memcpy(&data, d.runtime().memory.ram() + 0x440d44u, 4);
            if (data >= 0x100000u && data < 0x2000000u)
            {
                const uint32_t one = 1;
                std::memcpy(d.runtime().memory.ram() + data + 0x8cu, &one, 4);
            }
        }
        // AMP_AUTOPILOT=1: the game's own autopilot cheat (its debug toggle 0x110390 flips +0x100 of the object at
        // *0x435f14 and prints "CHEAT: autopilot mode"): the game plays the gems itself. For play tests of a whole song.
        if (static const bool autopilot = std::getenv("AMP_AUTOPILOT") != nullptr; autopilot)
        {
            uint32_t cheats;
            std::memcpy(&cheats, d.runtime().memory.ram() + 0x435f14u, 4);
            if (cheats >= 0x100000u && cheats < 0x2000000u)
            {
                const uint32_t one = 1;
                std::memcpy(d.runtime().memory.ram() + cheats + 0x100u, &one, 4);
            }
        }
        // AMP_WATCH=<address>[,<address>...]: a word of RAM printed at each frame end where it changed (finding what
        // overwrites a structure).
        if (static const char *watch = std::getenv("AMP_WATCH"); watch)
        {
            static std::vector<std::pair<uint32_t, uint32_t>> words;
            if (words.empty())
                for (const char *p = watch; p && *p;)
                {
                    words.emplace_back(static_cast<uint32_t>(std::strtoul(p, nullptr, 16)), 0xDEADBEEFu);
                    p = std::strchr(p, ',');
                    if (p)
                        ++p;
                }
            for (auto &[address, last] : words)
            {
                uint32_t v;
                std::memcpy(&v, d.runtime().memory.ram() + (address & 0x1FFFFFFu), 4);
                if (v != last)
                    std::printf("[amp:watch] frame %llu: %08x = %08x (was %08x)\n", (unsigned long long)f, address, v, last);
                last = v;
            }
        }
        pacer.frameEnd();
        amp::traceSound(f);
        if (f % 60 == 0)
            std::printf("[amp] frame=%llu pc=%x cycles=%llu\n", (unsigned long long)f, ctx->pc,
                        (unsigned long long)d.now());
    };
    bozo::audio::WavWriter recording;
    if (!wav.empty() && !recording.open(wav))
    {
        std::fprintf(stderr, "cannot open WAV %s\n", wav.c_str());
        return 2;
    }
#ifdef AMP_WITH_WINDOW
    bozo::app::DevicePad devices;
    bozo::app::AudioDevice audio;
    if (window && options.padScript.empty())
        host.libraries().pad.input = &devices;
#endif
    host.onAudio = [&](uint64_t, const int16_t *pcm, size_t count)
    {
        if (recording.isOpen())
            recording.write(pcm, count);
#ifdef AMP_WITH_WINDOW
        if (window && !mute)
            audio.push(pcm, count);
#endif
    };
    auto run = [&]()
    {
#ifdef AMP_WITH_WINDOW
        if (window && !mute)
            audio.open();
#endif
        uint64_t frames = 0;
        try
        {
            frames = host.run();
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "[amp] native failure: %s\n", e.what());
            return 1;
        }
        if (!dumpRam.empty())
            if (FILE *f = std::fopen(dumpRam.c_str(), "wb"))
            {
                std::fwrite(d.runtime().memory.ram(), 1, 0x2000000u, f);
                std::fclose(f);
            }
        std::printf("[amp] frames=%llu end=%s\n", (unsigned long long)frames, host.endReason().c_str());
        const auto &stats = host.stats();
        std::printf("[amp] compatibility: %llu register accesses, %llu unknown; %llu native VU0 calls\n",
                    (unsigned long long)stats.ioAccesses, (unsigned long long)stats.ioUnknown,
                    (unsigned long long)stats.vu0Calls);
        std::printf("[amp] calls: %llu kernel, %llu library, %llu port overrides\n",
                    (unsigned long long)stats.kernelCalls, (unsigned long long)stats.libraryCalls,
                    (unsigned long long)stats.overrideCalls);
        amp::reportRender();
        for (const auto &[name, count] : stats.unanswered)
            std::printf("[amp:unanswered] %10llu %s\n", (unsigned long long)count, name.c_str());
        if (std::getenv("AMP_CALL_STATS"))
        {
            std::vector<std::pair<uint64_t, std::string>> byName;
            for (const auto &[name, count] : stats.libraryByName)
                byName.emplace_back(count, name);
            std::sort(byName.rbegin(), byName.rend());
            for (const auto &[count, name] : byName)
                std::printf("[amp:lib] %10llu %s\n", (unsigned long long)count, name.c_str());
        }
        for (const auto &[site, counts] : stats.ioSites)
            std::printf("[amp:io] pc=%08x address=%08x reads=%llu writes=%llu %s\n", site.first, site.second,
                        (unsigned long long)counts[0], (unsigned long long)counts[1],
                        host.functionAt(site.first).c_str());
        return host.endReason().rfind("FAULT:", 0) == 0 ? 1 : 0;
    };
    int result;
#ifdef AMP_WITH_WINDOW
    if (window)
    {
        bozo::app::Game game;
        game.run = run;
        game.stop = [&] { host.requestStop(); };
        game.framesEnded = [&] { return host.framesEnded(); };
        game.saveState = [&](int slot) { host.requestSave(bozo::app::stateSlotPath(slot)); };
        game.loadState = [&](int slot) { host.requestLoad(bozo::app::stateSlotPath(slot)); };
        host.onStateMessage = [](bool, const std::string &message) { bozo::app::notify(message); };
        result = bozo::app::runWindow(windowOptions, game, &devices);
    }
    else
#endif
        result = run();
    recording.close();
    return result;
}
