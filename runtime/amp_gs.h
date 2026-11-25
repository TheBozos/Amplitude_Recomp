// Amplitude's GS translator: the GS registers the game's GIF packets set, its vertices drawn natively and its image (from Shinobido_WOTN_Recomp)
// transfers kept for the textures (runtime/amp_gs_state.cpp). The GIF and VIF parsing is runtime/amp_gs.cpp.
#pragma once
#include <cstdint>

namespace bozo::render
{
    struct Vertex;
}

namespace amp::gs
{
    // A GS register written with its raw value (PRIM 0x00 ... LABEL 0x62): A+D, REGLIST, a tag's PRE.
    void writeRegister(uint32_t address, uint64_t value);
    // A PACKED quadword for register descriptor `r` (0x0 PRIM ... 0xe A+D, 0xf NOP).
    void packed(uint32_t r, uint64_t lo, uint64_t hi);
    // A PACKED tag's data in VU1 data memory (XGKICK): loops x nreg quadwords from quadword `at`, wrapping at 1024.
    void packedRun(const uint8_t *vuMemory, uint32_t at, uint64_t regs, uint32_t nreg, uint32_t loops);
    // A REGLIST value for descriptor `r` (the register address, 0x0-0xd).
    void reglist(uint32_t r, uint64_t value);
    // An IMAGE transfer's start (the BITBLTBUF / TRXPOS / TRXREG set before it) and its quadwords.
    void imageBegin();
    void imageData(const uint8_t *quadword);
    void reportState();
    // A native mesh pass (runtime/amp_native.cpp), where its VU1 program would have run: triangles in the object's space
    // and `matrix` to the GS's primitive coordinates and Z (divided by w; columns first), drawn with `prim` (the pass's
    // GIF tag) and the GS state as it is now. u, v the texture's 0-1, fog and colour as the GS takes them.
    void nativeTriangles(uint64_t prim, const float matrix[16], const bozo::render::Vertex *v, uint32_t count);
    // The flips so far (for traces keyed by frame).
    uint64_t flips();
}
