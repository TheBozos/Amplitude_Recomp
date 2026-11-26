// Amplitude's native port boundary.
#pragma once
#include "bozo/libs/libraries.h"
#include "bozo/port.h"
#include "bozo/state.h"
#include <cstdint>

namespace amp
{
    // The port's overrides (registerFunction by address) and native instruction sites (runtime/amp_port.cpp).
    void install(Runtime &rt, bozo::port::Registry &registry);
    // The renderer's DMA channels, the MFIFO ring, the frame kick and the flip, native (runtime/amp_render.cpp).
    void installRender(Runtime &rt, bozo::port::Registry &registry);
    // A native site of the renderer's (a store to or load from a retired DMA/GS register): true when handled.
    bool renderSite(uint8_t *ram, R5900Context *ctx, Runtime *rt, uint32_t pc);
    // What the frames held, for the runner's report.
    void reportRender();
    // The Movie class's IPU pictures, native (runtime/amp_ipu.cpp): its functions and its register sites.
    void installIpu(Runtime &rt, bozo::port::Registry &registry);
    // The native renderer (runtime/amp_native.cpp): Harmonix's draws at their own functions.
    void installNative(Runtime &rt, bozo::port::Registry &registry);
    namespace native
    {
        // A VU1 program's start (MSCAL / MSCNT at `pcBytes`, its batch unpacked from `top` of VU1's data memory) answered
        // natively (runtime/amp_native.cpp, AMP_NATIVE=1): true when drawn here (the unit's continuation then `tpc`),
        // false for VU1 to run it.
        bool program(uint32_t pcBytes, const uint8_t *vuData, uint32_t top, uint32_t &tpc);
        void report();
    }
    bool ipuSite(uint8_t *ram, R5900Context *ctx, Runtime *rt, uint32_t pc);
    void reportIpu();
    // The engine's libraries (set by installAudio).
    bozo::libs::Libraries *libraries();
    // The IOP sound modules (runtime/amp_audio.cpp): module loads, SIF transfers and the mixer.
    void installAudio(bozo::libs::Libraries &libraries, uint8_t *eeRam);
    // The port's save-state section (runtime/amp_audio.cpp): the sound modules' IOP host, then the port's own
    // state below (the renderer's DMA registers and MFIFO ring, VIF and the VUs, the GS registers and local memory,
    // the IPU streams).
    void installState(bozo::state::Sections &sections);
    void serializeRender(bozo::state::Archive &a); // runtime/amp_render.cpp
    void serializeIpu(bozo::state::Archive &a);    // runtime/amp_ipu.cpp
    void serializeGs(bozo::state::Archive &a);     // runtime/amp_gs_state.cpp
    void serializeVu(bozo::state::Archive &a);     // runtime/amp_vu1.cpp
    // AMP_TRACE_SOUND=<n> / AMP_TRACE_RPC=1: the mixer and the drivers' RPC calls (runtime/amp_audio.cpp).
    void traceSound(uint64_t frame);

    // The GS translator (runtime/amp_gs.cpp, amp_gs_state.cpp; from Shinobido_WOTN_Recomp): the GIF packets of the
    // game's transfers drawn natively.
    namespace gs
    {
        // GIF packets (tags and their data), as the GIF would take them from PATH3.
        void packets(const uint8_t *data, uint32_t bytes);
        // IMAGE data (whole quadwords) for the host-to-local transfer under way.
        void imageBytes(const uint8_t *data, uint32_t bytes);
        // One quadword of VIF1 DIRECT data (PATH2).
        void directQuadword(const uint8_t *quadword);
        // A VU1 XGKICK (PATH1): the packets at `address` (quadwords) of VU1 data memory, up to EOP.
        void path1(const uint8_t *vuMemory, uint32_t address);
        // The display flip: the frame drawn so far is published (DISPFB of the buffer shown); the game frame ends.
        void flip(Runtime *rt, R5900Context *ctx, uint64_t dispfb, uint64_t display);
        // The frame's draws dropped (before a frame the port starts itself: a movie's picture).
        void dropDraws();
        void report();
    }

    // AMP_PROFILE=1: the game thread's time by part, exclusive (a scope pauses the one it interrupts), printed every
    // 250 flips with the flips' rate; "other" is the recompiled code, the libraries and the host.
    namespace prof
    {
        enum Bucket : int { Other, Vu1, Vu0, Gs, Decode, Submit, Vif, Upload, Count };
        bool on();
        void enter(Bucket b, Bucket &previous);
        void leave(Bucket previous);
        void flip();
        struct Scope
        {
            explicit Scope(Bucket b) : active(on()) { if (active) enter(b, previous); }
            ~Scope() { if (active) leave(previous); }
            bool active;
            Bucket previous = Other;
        };
    }

    // PATH3 held behind VIF1's MSKPATH3: the GIF chain sent with a VIF1 chain goes to the GS one EOP packet group at
    // each unmask, between the draws, as the GIF lets it through.
    namespace path3
    {
        void unmasked();
    }

    // VIF1 codes and the VU1 (runtime/amp_vu1.cpp): DIRECT / DIRECTHL go to the GS translator, UNPACK and MPG fill the
    // VU1's memories, MSCAL runs its programs (their XGKICKs go to the GS translator).
    namespace vif
    {
        // tagWords: the two VIF codes of a DMA tag's upper half (TTE).
        void codes(const uint8_t *data, uint32_t bytes, bool tagWords);
        // VIF0 codes (MPG and UNPACK to VU0's memories, MSCAL runs a VU0 program).
        void codes0(Runtime *rt, const uint8_t *data, uint32_t bytes);
        void report();
        uint64_t vu1Pairs();
        uint64_t vu1Programs();
        // The EE's RAM: where VIF data comes from, for the VU1 survey.
        inline const uint8_t *eeRam = nullptr;
    }
    // VU0 microprograms (runtime/amp_vu1.cpp's interpreter): the program at pcBytes of the code VIF0 loaded, on the
    // EE's COP2 registers and the runtime's VU0 data memory (VCALLMS).
    namespace vu0
    {
        void run(Runtime *rt, R5900Context *ctx, uint32_t pcBytes);
        // Its 4 KB code memory (the EE's view at 0x11000000: port::Registry::vu0CodeMemory).
        uint8_t *codeMemory();
    }
} // namespace amp
