// Amplitude's sound modules: Sony's LIBSD, SDRDRV and SCRTCHPD and Harmonix's SYNTH_S (the synthesizer) and SOFTFX_S
// (the effects), statically compiled from the player's disc (tools/amp_bozo_audio.py) and run by BozoEngine's native IRX
// host. The game reaches them through SIF RPC (libsdr's sceSdRemote: SDRDRV's 0x80000701; SYNTH_S's 0x75433178,
// SOFTFX_S's 0x75433776) as on the console's IOP; no IOP is modelled. From Shinobido_WOTN_Recomp's
// runtime/shino_audio.cpp.
#include "amp_audio_modules.h"
#include "amp_port.h"
#include "bozo/audio/mixer.h"
#include "bozo/libs/guest.h"
#include "bozo/libs/libraries.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace amp
{
    namespace
    {
        std::unique_ptr<bozo::native::irx::CompiledLibraries> sound;
        bozo::audio::Mixer *g_mixer = nullptr; // the one advanceTo hands back (AMP_TRACE_SOUND)
        bozo::libs::Libraries *g_libraries = nullptr;
        // AMP_TRACE_RPC=1: the drivers' RPC calls by service and command, every 250 frames (each server wrapped once,
        // when the trace first sees it: the modules register theirs as the game loads them).
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_rpcCalls;
        std::set<uint32_t> g_wrapped;
        void wrapServers()
        {
            for (auto &[service, handler] : g_libraries->sif.servers)
                if (g_wrapped.insert(service).second)
                    handler = [service, inner = handler](bozo::libs::Libraries &l, bozo::libs::Guest &g,
                                                         const bozo::libs::NativeRpcRequest &r) {
                        ++g_rpcCalls[{service, r.command}];
                        bool empty = true; // counted apart: the calls whose send buffer holds anything (command | 1)
                        for (uint32_t i = 0; i + 4 <= r.sendBytes && empty; i += 4)
                            empty = g.read32(r.send + i) == 0;
                        if (!empty && r.sendBytes)
                            ++g_rpcCalls[{service, r.command | 1u}];
                        // AMP_TRACE_RPC_DUMP=<frame>: each call's send buffer from that frame on (AMP_TRACE_RPC_MAX calls,
                        // 16 by default); AMP_TRACE_RPC_SKIP=<hex>: not the calls whose first word is that (0x6e: MIDI).
                        static const char *dump = std::getenv("AMP_TRACE_RPC_DUMP");
                        static int dumped = 0;
                        static const int most = std::getenv("AMP_TRACE_RPC_MAX") ? std::atoi(std::getenv("AMP_TRACE_RPC_MAX")) : 16;
                        static const char *skip = std::getenv("AMP_TRACE_RPC_SKIP");
                        static const char *only = std::getenv("AMP_TRACE_RPC_SERVICE"); // one service's calls alone
                        if (dump && l.framesEnded >= std::strtoull(dump, nullptr, 10) && dumped < most &&
                            (!only || std::strtoul(only, nullptr, 16) == service) &&
                            !(skip && r.sendBytes >= 4 && g.read32(r.send) == std::strtoul(skip, nullptr, 16)))
                        {
                            ++dumped;
                            std::printf("[amp:rpc] frame %llu %x/%x send %x (%u bytes) receive %x (%u):",
                                        (unsigned long long)l.framesEnded, service, r.command, r.send,
                                        r.sendBytes, r.receive, r.receiveBytes);
                            for (uint32_t i = 0; i < r.sendBytes && i < 512; i += 4)
                                std::printf(" %08x", g.read32(r.send + i));
                            std::printf("|end\n");
                        }
                        inner(l, g, r);
                    };
        }
    }

    bozo::libs::Libraries *libraries() { return g_libraries; }

    void traceSound(uint64_t frame)
    {
        static const bool rpc = std::getenv("AMP_TRACE_RPC") != nullptr;
        if (rpc && g_libraries)
        {
            wrapServers();
            if (frame % 250 == 0 && !g_rpcCalls.empty())
            {
                std::printf("[amp:rpc] frame %llu:", (unsigned long long)frame);
                for (const auto &[key, count] : g_rpcCalls)
                    std::printf(" %x/%x=%llu", key.first, key.second, (unsigned long long)count);
                std::printf("\n");
                g_rpcCalls.clear();
            }
        }
        static const bool on = std::getenv("AMP_TRACE_SOUND") != nullptr;
        static const uint64_t every = std::getenv("AMP_TRACE_SOUND") ? std::max(1ull, std::strtoull(std::getenv("AMP_TRACE_SOUND"), nullptr, 10)) : 50;
        if (!on || !g_mixer || frame % every != 0)
            return;
        const auto s = g_mixer->stats();
        static uint64_t keyOnsBefore = 0;
        std::printf("[amp:sound] frame %llu: %llu key ons, %d voices playing, peak %d", (unsigned long long)frame,
                    (unsigned long long)(s.keyOns - keyOnsBefore), s.voicesPlaying, s.peak);
        keyOnsBefore = s.keyOns;
        // The input (the movies' PCM) and the volumes and mixes it goes through, per group (core).
        for (int g = 0; g < 2; ++g)
            std::printf("; core %d input %d vol %04x/%04x master %04x/%04x mix %03x", g, int(g_mixer->inputPlaying(g)),
                        g_mixer->inputVolume(g, 0), g_mixer->inputVolume(g, 1), g_mixer->masterVolume(g, 0),
                        g_mixer->masterVolume(g, 1), g_mixer->mix(g));
        std::printf("\n");
    }

    void installAudio(bozo::libs::Libraries &libraries, uint8_t *)
    {
        sound = std::make_unique<bozo::native::irx::CompiledLibraries>(libraries, kAmpAudioHeapStart);
        g_libraries = &libraries;
        addAmpAudioImages(*sound);
        libraries.audioProvider = [](uint64_t cycle)
        {
            g_mixer = sound->advanceTo(cycle);
            // softFX writes its mix (the song select's preview loops) into core 0's sound data input by ordinary
            // transfers, never starting a block transfer: the input is read from sound memory, as the SPU2 does.
            static bozo::audio::Mixer *configured = nullptr; // (each mixer the modules hand back, once)
            if (g_mixer && g_mixer != configured)
            {
                g_mixer->setMemoryInput(true);
                configured = g_mixer;
            }
            return g_mixer;
        };
        libraries.sif.transfer = [](uint32_t address, const uint8_t *bytes, uint32_t count)
        {
            // AMP_TRACE_SIF=<most>: the SIF DMA transfers into the IOP's memory (address, size, first bytes).
            static const char *trace = std::getenv("AMP_TRACE_SIF");
            static int traced = 0;
            if (trace && traced < std::atoi(trace))
            {
                ++traced;
                std::printf("[amp:sif] %x <- %u bytes:", address, count);
                for (uint32_t i = 0; i < count && i < 32; ++i)
                    std::printf(" %02x", bytes[i]);
                std::printf("\n");
            }
            sound->writeBuffer(address, bytes, count);
        };
        libraries.sif.allocateBuffer = [](uint32_t bytes) { return sound->allocateBuffer(bytes); };
        libraries.sif.freeBuffer = [](uint32_t address) { return sound->freeBuffer(address); };
        libraries.sif.resetProvider = [] { sound->reset(); };
        libraries.sif.moduleLoaderWithArguments = [](const std::string &path, const std::vector<std::string> &arguments)
        {
            auto name = path.substr(path.find_last_of("/\\") + 1);
            if (const auto version = name.find(';'); version != std::string::npos)
                name.resize(version);
            for (char &c : name)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            // Answered at their EE APIs by BozoEngine's native pad, multitap, memory-card and CD libraries.
            if (name == "SIO2MAN.IRX" || name == "PADMAN.IRX" || name == "MTAPMAN.IRX" || name == "MCMAN.IRX" ||
                name == "MCSERV.IRX" || name == "CDVDSTM.IRX")
                return 1;
            // Online play and its chat keyboard (the network stack, USB): out of scope; their EE clients are answered
            // where the game uses them (the keyboard: runtime/amp_port.cpp).
            if (name == "USBD.IRX" || name == "USBKB.IRX" || name == "DEV9.IRX" || name == "SMAP.IRX" ||
                name == "INET.IRX" || name == "INETCTL.IRX" || name == "NETCNF.IRX" || name == "PPP.IRX" ||
                name == "PPPOE.IRX" || name == "AN986.IRX" || name == "CXTMDM.IRX" || name == "SPDUART.IRX" ||
                name == "MSIFRPC.IRX" || name == "LIBNETB.IRX" || name == "EZNCTL_S.IRX" || name == "EZNCNF_S.IRX" ||
                name == "SNPROFIL.IRX")
                return 1;
            if (name == "LIBSD.IRX" || name == "SDRDRV.IRX" || name == "SCRTCHPD.IRX" || name == "SYNTH_S.IRX" ||
                name == "SOFTFX_S.IRX")
                return sound->startImage(name + ";1", arguments);
            std::printf("[amp:iop] module load not answered: %s\n", path.c_str());
            return -1;
        };
    }

    // Amplitude's save-state section: what the engine's own sections do not hold, the sound modules' IOP host (its
    // memory, tasks, RPC servers, libsd and the mixer). Without it a loaded state kept the IOP as it was: silent, and
    // the RPC servers the modules registered gone, so the next sceSdRemote went unanswered (a movie played after a
    // load waited for its sound for ever). The game sends its textures and VIF state again each frame, but its VU
    // microcode only once at boot: a load puts it back from the EE's RAM (vif::restoreBootMicrocode).
    void installState(bozo::state::Sections &sections)
    {
        sections.add({"amp",
                      [](bozo::state::Archive &a)
                      {
                          sound->serialize(a);
                          // The port's own state, nested (a state written before it has none: only the boot's VU
                          // microcode is put back then, and such a state does not run on).
                          if (!a.loading() || a.sectionLeft() > 0)
                          {
                              a.begin("port");
                              serializeRender(a);
                              serializeVu(a);
                              serializeGs(a);
                              serializeIpu(a);
                              a.end();
                          }
                          else
                              vif::restoreBootMicrocode();
                      },
                      [] { return sound ? sound->unsaveable() : std::string(); }});
    }
} // namespace amp
