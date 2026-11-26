// Amplitude's native renderer (in progress): Harmonix's draws answered at their own functions, their meshes, materials
// and matrices drawn through BozoEngine's 3D path instead of the VIF1 / VU1 / GIF packets they build.
// docs/RECOMP_NOTES.md ("Native renderer") maps the classes and functions read so far.
//
// The PS2 mesh's draw, 0x215f30 (a virtual method: vtable 0x3d2a70, slot 5): a visibility test 0x232b30, the skinning
// 0x215d18 (the world transform it returns), then for each of the material's passes the material's GS state
// (0x213698, the default material 0x213570) and the VU1 header and prebuilt packet (0x2155b0).
//
// A pass (0x2155b0) adds to the scratchpad's packet (its write position at 0x70000008): VIF NOPs and an UNPACK of
// the 21-quadword header for VU1 image 8 (MSCAL 0x63e), then a REF of the mesh's prebuilt packet (0x214150: its faces
// batched into VU1 memory with the vertices) that runs the program. The header, by quadword:
//   0 vf19, the clipping guard's scale    1-4 vf20-23, the matrix to clip space (columns)
//   5 vf13, the colour's scale (128)      6, 7 vf14 / vf15: screen = vf14 * clip / w + vf15 (x, y, z); fog = vf14.w * w + vf15.w
//   8-10 vf16-18, the texture's transform 11 the GIF tag (PRE: PRIM; ST, RGBAQ, XYZF2) 12 the clipped fans' tag
//   13 vi13 (the lighting sub-program: 0x260 three lights, 0x373 the ambient only, 0x3d7 none, 0x6c point lights), vi14, vi15 (the vertex colour scales the ambient, the lights)
//   14 vf25 the ambient                   15-17 vf26-28, the three lights' directions (columns)   18-20 vf29-31 their colours
// The mesh (its geometry at +0x140) holds the vertices at +0xe8 (64 bytes: the position, the normal, the colour RGBA
// as floats 0-1, u v) and the faces at +0xf8 (three u16 vertex numbers each); +0x154 set, the packet points at the
// vertices instead of copying them (not done here yet).
//
// AMP_NATIVE=1: each pass of image 8 drawn natively. The pass's triangles are made from the mesh as VU1 makes them
// (the lighting, the colour's scale, the texture's transform, the fog), the header's UNPACK left in place, the packet's
// REF emptied, and a mark (a VIF NOP with NUM 0xa5) put before the UNPACK: where the VIF reaches it, the GS translator
// draws the triangles with the pass's matrix and the GS state the material set (runtime/amp_gs_state.cpp).
#include "amp_functions.h"
#include "amp_gs.h"
#include "amp_port.h"
#include "bozo/render/renderer.h"
#include <algorithm>
#include <cmath>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

namespace amp
{
    namespace
    {
        using bozo::render::Vertex;

        // A pass made, waiting for its mark.
        struct Pass
        {
            uint32_t mark = ~0u;
            uint64_t prim = 0;
            float matrix[16] = {};
            std::vector<Vertex> vertices;
        };
        constexpr uint32_t kPasses = 4096;
        std::vector<Pass> g_passes(kPasses);
        uint32_t g_nextMark = 0;
        uint64_t g_made = 0, g_drawn = 0, g_missed = 0, g_left = 0; // AMP_NATIVE's report
        std::set<uint32_t> g_leftWhy;                              // the sub-programs and paths left to VU1

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x1FFFFFFu), 4);
            return v;
        }

        // AMP_TRACE_MESHES=<flip>: each mesh drawn at that flip (its object, once), then the game's own draw.
        void meshDraw(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            static const char *trace = std::getenv("AMP_TRACE_MESHES");
            if (trace && gs::flips() == std::strtoull(trace, nullptr, 10))
            {
                static std::set<uint32_t> seen;
                const uint32_t self = ctx->r[4].uw[0];
                if (seen.insert(self).second)
                {
                    std::printf("[amp:mesh] %08x:", self);
                    for (uint32_t o = 0; o < 0x1c0; o += 4)
                        std::printf(" %08x", rd32(ram, self + o));
                    std::printf("\n");
                }
            }
            func_0x215f30(ram, ctx, rt);
        }

        // A prebuilt packet's VIF1 codes read as VU1 would take them: each batch's UNPACKs into its memory (TOPS taken as
        // 0), then at its MSCAL / MSCNT the batch, handed to `onBatch` with that memory. False for a code not in such
        // packets, or when `onBatch` refuses its batch. `batchAtEnd`: the codes stop before the batch's MSCAL (a
        // multi-mesh's upload of its mesh's first batch, 0x2161b0): the batch is taken at their end.
        using Memory = uint32_t (*)[4];
        template <class OnBatch> bool decodeVif(const uint8_t *d, uint32_t bytes, OnBatch &&onBatch, bool batchAtEnd = false)
        {
            static uint32_t mem[1024][4];
            uint32_t cl = 1, wl = 1;
            for (uint32_t at = 0; at + 4 <= bytes;)
            {
                uint32_t code;
                std::memcpy(&code, d + at, 4);
                at += 4;
                const uint32_t cmd = (code >> 24) & 0x7Fu, imm = code & 0xFFFFu;
                uint32_t num = (code >> 16) & 0xFFu;
                if (cmd >= 0x60)
                {
                    const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u;
                    if (vl > 1u)
                        return false;
                    if (!num)
                        num = 256;
                    const uint32_t elemBytes = vl == 0 ? 4u : 2u, n = vn + 1u;
                    const uint32_t size = (num * n * elemBytes + 3u) & ~3u;
                    if (at + size > bytes)
                        return false;
                    const bool usn = (imm >> 14) & 1u;
                    for (uint32_t i = 0; i < num; ++i)
                    {
                        const uint32_t addr = ((imm & 0x3FFu) + (wl < cl ? (i / wl) * cl + i % wl : i)) & 0x3FFu;
                        for (uint32_t e = 0; e < n; ++e)
                        {
                            const uint8_t *src = d + at + (i * n + e) * elemBytes;
                            if (vl == 0)
                                std::memcpy(&mem[addr][e], src, 4);
                            else
                            {
                                uint16_t x;
                                std::memcpy(&x, src, 2);
                                mem[addr][e] = usn ? x : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(x)));
                            }
                        }
                    }
                    at += size;
                    continue;
                }
                switch (cmd)
                {
                case 0x00: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x10: case 0x11: case 0x13:
                    break; // NOP, OFFSET, BASE, ITOP, STMOD, MSKPATH3, MARK, FLUSHE, FLUSH, FLUSHA
                case 0x01: // STCYCL
                    cl = std::max(imm & 0xFFu, 1u);
                    wl = std::max((imm >> 8) & 0xFFu, 1u);
                    break;
                case 0x20: at += 4; break;             // STMASK
                case 0x30: case 0x31: at += 16; break; // STROW, STCOL
                case 0x14: case 0x15: case 0x17:       // MSCAL, MSCALF, MSCNT: the batch
                    if (!onBatch(Memory(mem)))
                        return false;
                    break;
                default:
                    return false;
                }
            }
            return !batchAtEnd || onBatch(Memory(mem));
        }

        // A triangle batch (0x214150's packets, image 8 and 0x3dc): the counts at 0x15 (vertices, faces), the vertices
        // from 0x16 (4 quadwords each: STCYCL 4 / 2 for the position and normal, 4 / 1 for the colour and u v), the
        // faces after them (V3-16: each vertex's quadword offset). `out`: three vertices a face, 16 floats each.
        bool decodeCodes(const uint8_t *d, uint32_t bytes, std::vector<std::array<float, 16>> &out, bool batchAtEnd = false)
        {
            out.clear();
            return decodeVif(
                d, bytes,
                [&](Memory mem) {
                    const uint32_t vertices = mem[0x15][0], faces = mem[0x15][1];
                    if (vertices > 250u || 0x16u + 4u * vertices + faces > 1024u)
                        return false;
                    for (uint32_t f = 0; f < faces; ++f)
                        for (uint32_t k = 0; k < 3; ++k)
                        {
                            const uint32_t offset = mem[0x16u + 4u * vertices + f][k] & 0xFFFFu;
                            if (offset / 4u >= vertices)
                                return false;
                            std::array<float, 16> v;
                            std::memcpy(v.data(), mem[0x16u + offset], 64);
                            out.push_back(v);
                        }
                    return true;
                },
                batchAtEnd);
        }

        // A packet in RAM, VIF codes all through (the REF of a pass whose packet copies the vertices).
        bool decodePacket(const uint8_t *ram, uint32_t packet, uint32_t qwc, std::vector<std::array<float, 16>> &out)
        {
            if ((packet & 0x1FFFFFFu) + qwc * 16u > 0x2000000u)
                return false;
            return decodeCodes(ram + (packet & 0x1FFFFFFu), qwc * 16u, out);
        }

        // A packet that points at the vertices, as copied into the scratchpad (`qwc` quadwords from `start`): a DMA chain
        // of CNT tags (VIF codes after the tag) and REFs (each vertex's quadwords, from the mesh's +0xe8, as they are
        // now), its VIF codes gathered in order (TTE off: the tags' upper halves are not codes).
        bool gatherChain(const uint8_t *ram, const uint8_t *spr, uint32_t start, uint32_t qwc, std::vector<uint8_t> &codes)
        {
            codes.clear();
            for (uint32_t q = 0; q < qwc;)
            {
                const uint8_t *tag = spr + ((start + 16u * q) & 0x3FF0u);
                uint32_t lo, addr;
                std::memcpy(&lo, tag, 4);
                std::memcpy(&addr, tag + 4, 4);
                const uint32_t n = lo & 0xFFFFu, id = (lo >> 28) & 7u;
                if (id == 1u) // cnt
                {
                    if (q + 1u + n > qwc)
                        return false;
                    for (uint32_t k = 0; k < n; ++k)
                    {
                        const uint8_t *d = spr + ((start + 16u * (q + 1u + k)) & 0x3FF0u);
                        codes.insert(codes.end(), d, d + 16);
                    }
                    q += 1u + n;
                }
                else if (id == 3u || id == 4u) // ref, refs
                {
                    const uint32_t a = addr & 0x1FFFFF0u;
                    if (a + 16u * n > 0x2000000u)
                        return false;
                    codes.insert(codes.end(), ram + a, ram + a + 16u * n);
                    ++q;
                }
                else
                    return false;
            }
            return true;
        }

        // A triangle packet copied into the scratchpad, read as above.
        bool decodeChain(const uint8_t *ram, const uint8_t *spr, uint32_t start, uint32_t qwc, std::vector<std::array<float, 16>> &out,
                         bool batchAtEnd = false)
        {
            static std::vector<uint8_t> codes;
            return gatherChain(ram, spr, start, qwc, codes) &&
                   decodeCodes(codes.data(), static_cast<uint32_t>(codes.size()), out, batchAtEnd);
        }

        // A line batch (0x214710's packets, VU1 program 0x5a8: a mesh's line pass, 0x2159b0): the counts at 8 (vertices,
        // lines), the positions from 9 (2 quadwords a vertex: the position, then the program's output), the lines
        // after them (V2-16: each end's quadword offset). Each position is transformed by the clip matrix `clip`
        // (columns) and tested against -w..w on x, y, z; a vertex's flags are its test's and the vertex's before it (the
        // clip flags as FCGET gives them, two tests); a line is drawn only if both ends' flags are clear, a batch not at
        // all if every vertex's flags share a bit. `out`: two positions a line (16 floats each, the position first).
        bool decodeLines(const uint8_t *d, uint32_t bytes, const float (*clip)[4], std::vector<std::array<float, 16>> &out)
        {
            out.clear();
            static std::vector<uint32_t> flags;
            return decodeVif(d, bytes, [&](Memory mem) {
                const uint32_t vertices = mem[8][0], lines = mem[8][1];
                if (vertices > 500u || 9u + 2u * vertices + lines > 1024u)
                    return false;
                flags.resize(vertices);
                uint32_t previous = 0, all = 0xFFFu;
                for (uint32_t k = 0; k < vertices; ++k)
                {
                    float p[4];
                    std::memcpy(p, mem[9u + 2u * k], 16);
                    float c[4];
                    for (int r = 0; r < 4; ++r)
                        c[r] = clip[0][r] * p[0] + clip[1][r] * p[1] + clip[2][r] * p[2] + clip[3][r];
                    const float w = std::fabs(c[3]);
                    const uint32_t test = (c[0] > w ? 1u : 0u) | (c[0] < -w ? 2u : 0u) | (c[1] > w ? 4u : 0u) |
                                          (c[1] < -w ? 8u : 0u) | (c[2] > w ? 16u : 0u) | (c[2] < -w ? 32u : 0u);
                    flags[k] = test | (previous << 6);
                    previous = test;
                    all &= flags[k];
                }
                if (all)
                    return true; // the batch not drawn
                for (uint32_t l = 0; l < lines; ++l)
                {
                    const uint32_t a = (mem[9u + 2u * vertices + l][0] & 0xFFFFu) / 2u, b = (mem[9u + 2u * vertices + l][1] & 0xFFFFu) / 2u;
                    if (a >= vertices || b >= vertices)
                        return false;
                    if (flags[a] | flags[b])
                        continue;
                    for (const uint32_t k : {a, b})
                    {
                        std::array<float, 16> v{};
                        std::memcpy(v.data(), mem[9u + 2u * k], 16);
                        out.push_back(v);
                    }
                }
                return true;
            });
        }

        // The triangles cut to the clip space's depth, -w <= z <= w, as VU1's clipper cuts them (its x and y guard band
        // the GPU's own clipping answers): past it the GS Z wraps, and a surface reaching behind the camera (the
        // arenas' beams over the screen's corners) failed the depth test. The clip space is linear in the object's
        // space, so the cut vertices are the object's points and their values between the two ends.
        // `c`: the clip matrix's four columns.
        void clipDepth(const float (*c)[4], std::vector<Vertex> &tris)
        {
            auto dist = [&](const Vertex &v, float sign) {
                const float z = c[0][2] * v.x + c[1][2] * v.y + c[2][2] * v.z + c[3][2];
                const float w = c[0][3] * v.x + c[1][3] * v.y + c[2][3] * v.z + c[3][3];
                return w + sign * z;
            };
            auto lerp = [](const Vertex &a, const Vertex &b, float t) {
                Vertex r;
                r.x = a.x + (b.x - a.x) * t;
                r.y = a.y + (b.y - a.y) * t;
                r.z = a.z + (b.z - a.z) * t;
                r.u = a.u + (b.u - a.u) * t;
                r.v = a.v + (b.v - a.v) * t;
                r.fog = a.fog + (b.fog - a.fog) * t;
                r.q = 1.0f;
                r.rgba = 0;
                for (int k = 0; k < 4; ++k)
                {
                    const float ca = float((a.rgba >> (8 * k)) & 0xFFu), cb = float((b.rgba >> (8 * k)) & 0xFFu);
                    r.rgba |= static_cast<uint32_t>(ca + (cb - ca) * t + 0.5f) << (8 * k);
                }
                return r;
            };
            bool any = false;
            for (const Vertex &v : tris)
                if (dist(v, 1.0f) < 0.0f || dist(v, -1.0f) < 0.0f)
                {
                    any = true;
                    break;
                }
            if (!any)
                return;
            static std::vector<Vertex> out;
            out.clear();
            for (size_t t = 0; t + 2 < tris.size(); t += 3)
            {
                Vertex poly[9], next[9];
                uint32_t n = 3;
                std::copy(tris.begin() + t, tris.begin() + t + 3, poly);
                for (const float sign : {1.0f, -1.0f})
                {
                    uint32_t m = 0;
                    for (uint32_t i = 0; i < n; ++i)
                    {
                        const Vertex &a = poly[i], &b = poly[(i + 1) % n];
                        const float da = dist(a, sign), db = dist(b, sign);
                        if (da >= 0.0f)
                            next[m++] = a;
                        if ((da >= 0.0f) != (db >= 0.0f))
                            next[m++] = lerp(a, b, da / (da - db));
                    }
                    n = m;
                    std::copy(next, next + n, poly);
                    if (n < 3)
                        break;
                }
                for (uint32_t i = 1; n >= 3 && i + 1 < n; ++i)
                {
                    out.push_back(poly[0]);
                    out.push_back(poly[i]);
                    out.push_back(poly[i + 1]);
                }
            }
            tris.swap(out);
        }

        // How a program colours and maps its vertices: the lighting sub-program (vi13 or vi12: 0x260 three lights, 0x373
        // the ambient only, 0x3d7 none, 0x6c a point light) and its switches (vi14 the vertex colour times the ambient,
        // vi15 times the lights), the lights (vf25-31), the colour's scale and the texture's transform.
        struct Shading
        {
            uint32_t sub = 0;
            bool colourAmbient = false, colourLights = false;
            const float *amb, *l0, *l1, *l2, *c0, *c1, *c2; // vf25, vf26-28, vf29-31
            const float *scale, *t0, *t1, *t2;               // the colour's scale, the texture's transform
            bool known() const { return sub == 0x260u || sub == 0x373u || sub == 0x3d7u || sub == 0x6cu; }
        };

        // A vertex (16 floats: the position, the normal, the colour 0-1, u v) as the program leaves it, but the fog.
        void shade(const Shading &sh, const float *q, Vertex &v)
        {
            v.x = q[0];
            v.y = q[1];
            v.z = q[2];
            float col[4] = {q[8], q[9], q[10], q[11]};
            const float *amb = sh.amb, *l0 = sh.l0, *l1 = sh.l1, *l2 = sh.l2, *c0 = sh.c0, *c1 = sh.c1, *c2 = sh.c2;
            auto mix = [&](int k, float light) {
                return std::min((sh.colourLights ? light * q[8 + k] : light) + (sh.colourAmbient ? amb[k] * q[8 + k] : amb[k]), 1.0f);
            };
            if (sh.sub == 0x373u) // the ambient alone, or times the vertex's colour
                for (int k = 0; k < 4; ++k)
                    col[k] = std::min(sh.colourAmbient ? amb[k] * q[8 + k] : amb[k], 1.0f);
            else if (sh.sub == 0x260u)
            {
                const float nx = q[4], ny = q[5], nz = q[6];
                const float d0 = std::max(0.0f, l0[0] * nx + l1[0] * ny + l2[0] * nz);
                const float d1 = std::max(0.0f, l0[1] * nx + l1[1] * ny + l2[1] * nz);
                const float d2 = std::max(0.0f, l0[2] * nx + l1[2] * ny + l2[2] * nz);
                for (int k = 0; k < 4; ++k)
                    col[k] = mix(k, c0[k] * d0 + c1[k] * d1 + c2[k] * d2);
            }
            else if (sh.sub == 0x6cu)
            {
                // A point light at (vf26.x, vf27.x, vf28.x) within vf27.w of the vertex (squared), its colour vf29 times
                // (L - P) . N times (vf26.w + 1 / |L - P|); nothing past the range or behind the surface.
                const float lx = l0[0] - q[0], ly = l1[0] - q[1], lz = l2[0] - q[2];
                const float d2 = lx * lx + ly * ly + lz * lz, dot = lx * q[4] + ly * q[5] + lz * q[6];
                const float f = d2 <= l1[3] && dot >= 0.0f && d2 > 0.0f ? dot * (l0[3] + 1.0f / std::sqrt(d2)) : 0.0f;
                for (int k = 0; k < 4; ++k)
                    col[k] = mix(k, c0[k] * f);
            }
            uint32_t rgba = 0;
            for (int k = 0; k < 4; ++k)
            {
                const float x = col[k] * sh.scale[k]; // ftoi0: toward zero
                const uint32_t b = x <= 0.0f ? 0u : x >= 255.0f ? 255u : static_cast<uint32_t>(x);
                rgba |= b << (8 * k);
            }
            v.rgba = rgba;
            v.u = sh.t0[0] * q[12] + sh.t1[0] * q[13] + sh.t2[0];
            v.v = sh.t0[1] * q[12] + sh.t1[1] * q[13] + sh.t2[1];
            v.q = 1.0f;
        }

        // The matrix to the GS's coordinates from a clip matrix (columns `c`): screen = scale * clip + offset * clip.w,
        // over clip.w.
        void toScreen(const float (*c)[4], const float *scale, const float *offset, float m[16])
        {
            for (int k = 0; k < 4; ++k)
            {
                m[4 * k + 0] = scale[0] * c[k][0] + offset[0] * c[k][3];
                m[4 * k + 1] = scale[1] * c[k][1] + offset[1] * c[k][3];
                m[4 * k + 2] = scale[2] * c[k][2] + offset[2] * c[k][3];
                m[4 * k + 3] = c[k][3];
            }
        }

        // A GIF tag's PRIM (PRE set), or false.
        bool tagPrim(const float *tagQuadword, uint64_t &prim)
        {
            uint64_t tag;
            std::memcpy(&tag, tagQuadword, 8);
            if (!((tag >> 46) & 1u))
                return false;
            prim = (tag >> 47) & 0x7FFu;
            return true;
        }

        // The pass's triangles as VU1 image 8 makes them, from the header `h` (21 quadwords) and the mesh's geometry.
        bool makePass(const std::vector<std::array<float, 16>> &raw, const float (*h)[4], Pass &p)
        {
            uint32_t sub, vi14, vi15;
            std::memcpy(&sub, h[13], 4);
            std::memcpy(&vi14, &h[13][1], 4);
            std::memcpy(&vi15, &h[13][2], 4);
            const Shading sh{sub & 0xFFFFu, (vi14 & 0xFFFFu) != 0, (vi15 & 0xFFFFu) != 0, h[14], h[15], h[16], h[17],
                             h[18], h[19], h[20], h[5], h[8], h[9], h[10]};
            if (!sh.known())
            {
                g_leftWhy.insert(sh.sub);
                return false;
            }
            if (!tagPrim(h[11], p.prim))
            {
                g_leftWhy.insert(0x40000u); // no PRIM in the tag
                return false;
            }
            const float *s = h[6], *o = h[7];
            toScreen(&h[1], s, o, p.matrix);
            p.vertices.resize(raw.size());
            for (size_t i = 0; i < raw.size(); ++i)
            {
                Vertex &v = p.vertices[i];
                shade(sh, raw[i].data(), v);
                // The fog: vf14.w * clip.w + vf15.w, 0-255 (not over w).
                const float w = h[1][3] * v.x + h[2][3] * v.y + h[3][3] * v.z + h[4][3];
                const float f = std::clamp(s[3] * w + o[3], 0.0f, 255.0f);
                v.fog = float(static_cast<uint32_t>(f)) * (1.0f / 255.0f);
            }
            clipDepth(&h[1], p.vertices);
            return true;
        }

        // A pass made from its header at `head` (the scratchpad) and its triangles' vertices; the mark written at
        // `markAt` (a VIF code's place in the scratchpad). False (left to VU1) when the pass is not one this reads.
        bool markPass(uint8_t *spr, uint32_t head, const std::vector<std::array<float, 16>> &raw, uint32_t markAt)
        {
            float h[21][4];
            for (uint32_t k = 0; k < 21; ++k)
                std::memcpy(h[k], spr + ((head + 16u + 16u * k) & 0x3FF0u), 16);
            Pass &p = g_passes[g_nextMark % kPasses];
            if (!makePass(raw, h, p))
            {
                ++g_left;
                return false;
            }
            p.mark = g_nextMark & 0xFFFFu;
            const uint32_t mark = 0x00A50000u | p.mark;
            std::memcpy(spr + (markAt & 0x3FFCu), &mark, 4);
            ++g_nextMark;
            ++g_made;
            return true;
        }

        // A pass whose packet points at the vertices, under way (its header in the scratchpad): taken at the flush.
        struct PointerPass
        {
            uint32_t mesh = 0, head = 0;
        } g_pointerPass;

        // A mesh pass (0x2155b0: a0 the mesh, its geometry at +0x140): the game's own, then, with AMP_NATIVE, made
        // native and its program's run taken out of the packet. AMP_TRACE_PASSES=<flip>: the packet each adds.
        void meshPass(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            static const bool native = std::getenv("AMP_NATIVE") && std::atoi(std::getenv("AMP_NATIVE")) != 0;
            static const char *trace = std::getenv("AMP_TRACE_PASSES");
            uint8_t *spr = rt->memory.scratchpad();
            uint32_t p0;
            std::memcpy(&p0, spr + 8, 4);
            const uint32_t self = ctx->r[4].uw[0];
            if (native && rd32(ram, rd32(ram, self + 0x140) + 0x154) != 0)
                g_pointerPass = {self, p0};
            func_0x2155b0(ram, ctx, rt);
            g_pointerPass = {};
            uint32_t p1;
            std::memcpy(&p1, spr + 8, 4);
            if (trace && gs::flips() == std::strtoull(trace, nullptr, 10))
            {
                static int shown = 0;
                if (++shown <= 6)
                {
                    std::printf("[amp:pass] mesh %08x chain %08x..%08x\n", self, p0, p1);
                    for (uint32_t a = p0; a < p1 && a - p0 < 0x400; a += 16)
                    {
                        uint32_t w[4];
                        std::memcpy(w, spr + (a & 0x3FF0u), 16);
                        std::printf("  %04x: %08x %08x %08x %08x\n", a & 0x3FFFu, w[0], w[1], w[2], w[3]);
                    }
                }
            }
            if (!native || p1 <= p0 || p1 - p0 > 0x1000u)
                return;
            auto word = [&](uint32_t a) {
                uint32_t v;
                std::memcpy(&v, spr + (a & 0x3FFCu), 4);
                return v;
            };
            // The header's UNPACK (V4-32, 21 quadwords, at TOPS) in the first or second quadword.
            uint32_t head = p0;
            if (word(head + 12) != 0x6c158000u)
                head += 16;
            if (word(head + 12) != 0x6c158000u || word(head) != 0 || head + 16u * 22u > p1)
            {
                g_leftWhy.insert(0x20000u); // no header where expected
                ++g_left;
                return;
            }
            const uint32_t geometry = rd32(ram, self + 0x140);
            if (rd32(ram, geometry + 0x154) != 0)
                return; // taken at its flush (ringFlush)
            // The packet's REF (ID 3, the prebuilt packet's quadwords) after the header.
            const uint32_t qwc = rd32(ram, geometry + 0x16c) & 0xFFFFu;
            uint32_t ref = 0;
            for (uint32_t a = head + 16u * 22u; a + 16 <= p1; a += 16)
                if (((word(a) >> 28) & 7u) == 3u && (word(a) & 0xFFFFu) == qwc)
                {
                    ref = a;
                    break;
                }
            if (!ref)
            {
                g_leftWhy.insert(0x30000u); // no REF of the packet
                ++g_left;
                return;
            }
            static std::vector<std::array<float, 16>> raw;
            if (!decodePacket(ram, word(ref + 4), qwc, raw))
            {
                g_leftWhy.insert(0x50000u); // a packet not read
                ++g_left;
                return;
            }
            if (!markPass(spr, head, raw, head))
                return;
            const uint32_t emptied = word(ref) & ~0xFFFFu;
            std::memcpy(spr + (ref & 0x3FF0u), &emptied, 4);
        }

        // A PsMultiMesh's draw under way (0x21a4d0, the class's draw at vtable 0x3d2d48 slot 3 by 0x21aa40): one mesh
        // drawn at each of its instances, by VU1 program 0x3dc. The header (19 quadwords: vf16-19 the camera's clip
        // matrix, vf23 the colour's scale, vf24 / vf15 the viewport's scale and offset, vf20-22 the texture's transform,
        // the GIF tag, vi12 / vi14 the lighting sub-program and its switch, vf25-31 the lights), then the mesh's first
        // batch (its packet's first +0x174 quadwords, 0x2161b0: no MSCAL of its own), then chunks of instances, each
        // flushed (0x211128) with its MSCAL 0x3dc (the first) or MSCNT: an UNPACK of the count and each instance's 4
        // quadwords (its x, y, z axes and its position, REFs to the instance list's nodes at +0x10). The program lights
        // and maps the vertices once, then transforms them by clip matrix times instance, no fog (F 0).
        struct MultiDraw
        {
            uint32_t self = 0, p0 = 0;
            uint32_t copyTo = 0, copyQwc = 0; // a pointer packet's batch copied into the scratchpad (0x212500)
            bool made = false, skip = false;
            float h[19][4];
            uint64_t prim = 0;
            float matrix[16];
            std::vector<Vertex> tris; // the mesh's triangles, shaded, in its own space
        } g_multi;

        // At a flush inside a multi-mesh's draw: the first one reads the header and the mesh; each one with a chunk
        // (its MSCAL / MSCNT last) makes the chunk's instances native and puts the mark in the program's place.
        void multiFlush(const uint8_t *ram, uint8_t *spr)
        {
            MultiDraw &m = g_multi;
            uint32_t end;
            std::memcpy(&end, spr + 8, 4);
            auto word = [&](uint32_t a) {
                uint32_t v;
                std::memcpy(&v, spr + (a & 0x3FFCu), 4);
                return v;
            };
            auto leave = [&](uint32_t why) {
                g_leftWhy.insert(why);
                ++g_left;
                m.skip = true;
            };
            if (!m.made && !m.skip)
            {
                const uint32_t mesh = rd32(ram, m.self + 0x18), geometry = rd32(ram, mesh + 0x140);
                uint32_t head = m.p0;
                if (word(head + 12) != 0x6c138000u)
                    head += 16;
                if (word(head + 12) != 0x6c138000u || head + 16u * 20u > end)
                    return leave(0x70000u); // no header where expected
                const uint32_t qwc = rd32(ram, geometry + 0x174) & 0xFFFFu;
                static std::vector<std::array<float, 16>> raw;
                if (rd32(ram, geometry + 0x154) != 0)
                {
                    // A packet pointing at the vertices: its batch, a DMA chain, copied into the scratchpad.
                    if (!m.copyTo || m.copyQwc != qwc || (m.copyTo & 0x3FF0u) + 16u * qwc > 0x4000u ||
                        !decodeChain(ram, spr, m.copyTo, qwc, raw, true))
                        return leave(0x70001u);
                }
                else
                {
                    uint32_t ref = 0;
                    for (uint32_t a = head + 16u * 20u; a + 16 <= end; a += 16)
                        if (((word(a) >> 28) & 7u) == 3u && (word(a) & 0xFFFFu) == qwc)
                        {
                            ref = a;
                            break;
                        }
                    const uint32_t packet = ref ? word(ref + 4) & 0x1FFFFF0u : 0;
                    if (!ref || packet + 16u * qwc > 0x2000000u || !decodeCodes(ram + packet, 16u * qwc, raw, true))
                        return leave(0x70002u); // the mesh's batch not read
                }
                for (uint32_t k = 0; k < 19; ++k)
                    std::memcpy(m.h[k], spr + ((head + 16u + 16u * k) & 0x3FF0u), 16);
                uint32_t sub, vi14;
                std::memcpy(&sub, m.h[11], 4);
                std::memcpy(&vi14, &m.h[11][1], 4);
                // vi15 is not in this header (the program keeps the unit's): taken as clear, as image 8 leaves it.
                const Shading sh{sub & 0xFFFFu, (vi14 & 0xFFFFu) != 0, false, m.h[12], m.h[13], m.h[14], m.h[15],
                                 m.h[16], m.h[17], m.h[18], m.h[4], m.h[7], m.h[8], m.h[9]};
                if (!sh.known())
                    return leave(sh.sub | 0x80000u);
                if (!tagPrim(m.h[10], m.prim))
                    return leave(0x40000u);
                toScreen(&m.h[0], m.h[5], m.h[6], m.matrix);
                m.tris.resize(raw.size());
                for (size_t i = 0; i < raw.size(); ++i)
                {
                    shade(sh, raw[i].data(), m.tris[i]);
                    m.tris[i].fog = 0.0f;
                }
                m.made = true;
            }
            if (!m.made)
                return;
            // The chunk: its program start in the last quadword, the instances' REFs (qwc 4) before it (a CNT tag
            // or two between).
            const uint32_t last = end - 16u, start = word(last);
            if ((start != 0x140003dcu && start != 0x17000000u) || word(last + 4) || word(last + 8) || word(last + 12))
                return;
            static std::vector<uint32_t> instances;
            instances.clear();
            uint32_t a = last - 16u;
            for (int skipped = 0; skipped < 3 && word(a) != 0x30000004u; ++skipped)
                a -= 16u;
            for (; word(a) == 0x30000004u && instances.size() < 256; a -= 16u)
                instances.push_back(word(a + 4) & 0x1FFFFF0u);
            if (instances.empty())
                return leave(0x70003u);
            Pass &p = g_passes[g_nextMark % kPasses];
            p.prim = m.prim;
            std::memcpy(p.matrix, m.matrix, sizeof p.matrix);
            p.vertices.resize(m.tris.size() * instances.size());
            size_t o = 0;
            for (auto it = instances.rbegin(); it != instances.rend(); ++it) // (gathered last first)
            {
                float t[16];
                std::memcpy(t, ram + *it, 64);
                for (const Vertex &v : m.tris)
                {
                    Vertex &w = p.vertices[o++];
                    w = v;
                    w.x = t[0] * v.x + t[4] * v.y + t[8] * v.z + t[12];
                    w.y = t[1] * v.x + t[5] * v.y + t[9] * v.z + t[13];
                    w.z = t[2] * v.x + t[6] * v.y + t[10] * v.z + t[14];
                }
            }
            clipDepth(&m.h[0], p.vertices);
            p.mark = g_nextMark & 0xFFFFu;
            const uint32_t mark = 0x00A50000u | p.mark;
            std::memcpy(spr + (last & 0x3FF0u), &mark, 4);
            ++g_nextMark;
            ++g_made;
        }

        // A transfer to the scratchpad started (0x212500: a0 its descriptor, +4 the source, +8 its end; a2 where to). In a
        // multi-mesh's draw: where its mesh's batch goes (a packet that points at the vertices).
        void sprTransfer(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            if (g_multi.self && !g_multi.copyTo)
            {
                const uint32_t desc = ctx->r[4].uw[0], from = rd32(ram, desc + 4), to = rd32(ram, desc + 8);
                g_multi.copyTo = ctx->r[6].uw[0];
                g_multi.copyQwc = to > from ? (to - from) / 16u : 0;
            }
            func_0x212500(ram, ctx, rt);
        }

        // The multi-mesh's draw (0x21a4d0): with AMP_NATIVE, its flushes are watched (multiFlush).
        void multiDraw(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            static const bool native = std::getenv("AMP_NATIVE") && std::atoi(std::getenv("AMP_NATIVE")) != 0;
            if (native)
            {
                g_multi.self = ctx->r[4].uw[0];
                std::memcpy(&g_multi.p0, rt->memory.scratchpad() + 8, 4);
                g_multi.made = g_multi.skip = false;
                g_multi.copyTo = g_multi.copyQwc = 0;
            }
            func_0x21a4d0(ram, ctx, rt);
            g_multi.self = 0;
        }

        // A mesh's line pass (0x2159b0, from the mesh's draw 0x215f30: a0 the mesh): VU1 program 0x5a8 draws the
        // packet at the geometry's +0x168 (0x214710: its lines, pairs of u16 at +0x108, over the vertices; +0x16e
        // quadwords) with an 8-quadword header: vf28-31 the clip matrix, the GIF tag, vf26 / vf27 the viewport's scale
        // and offset (w: the fog's, fog = vf26.w * w + vf27.w, 0-255), vf25 the lines' one colour (RGBA as integers,
        // the CPU's 255ths and 128ths). As the mesh pass: the packet's REF emptied and the mark before the header, or
        // for a packet that points at the vertices, taken at its flush.
        bool makeLines(const uint8_t *codes, uint32_t bytes, const float (*h)[4], Pass &p)
        {
            if (!tagPrim(h[4], p.prim))
                return false;
            static std::vector<std::array<float, 16>> raw;
            if (!decodeLines(codes, bytes, &h[0], raw))
                return false;
            // Each line as the GS path draws a LINE: a quad one GS pixel tall, from the ends projected here (every end
            // kept is inside the clip space: w > 0), drawn as triangles with an identity matrix (GS coordinates and Z).
            float m[16];
            toScreen(&h[0], h[5], h[6], m);
            static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::memcpy(p.matrix, kIdentity, sizeof kIdentity);
            p.prim = (p.prim & ~7ull) | 3u;
            uint32_t c[4];
            std::memcpy(c, h[7], 16);
            const uint32_t rgba = (c[0] & 0xFFu) | (c[1] & 0xFFu) << 8 | (c[2] & 0xFFu) << 16 | (c[3] & 0xFFu) << 24;
            auto end = [&](const std::array<float, 16> &q) {
                Vertex v;
                const float x = q[0], y = q[1], z = q[2];
                const float w = m[3] * x + m[7] * y + m[11] * z + m[15];
                v.x = (m[0] * x + m[4] * y + m[8] * z + m[12]) / w;
                v.y = (m[1] * x + m[5] * y + m[9] * z + m[13]) / w;
                v.z = (m[2] * x + m[6] * y + m[10] * z + m[14]) / w;
                v.rgba = rgba;
                v.u = v.v = 0.0f;
                v.q = 1.0f;
                const float cw = h[0][3] * x + h[1][3] * y + h[2][3] * z + h[3][3];
                v.fog = float(static_cast<uint32_t>(std::clamp(h[5][3] * cw + h[6][3], 0.0f, 255.0f))) * (1.0f / 255.0f);
                return v;
            };
            p.vertices.clear();
            for (size_t i = 0; i + 1 < raw.size(); i += 2)
            {
                const Vertex a = end(raw[i]), b = end(raw[i + 1]);
                Vertex a2 = a, b2 = b;
                a2.y += 1.0f;
                b2.y += 1.0f;
                for (const Vertex &v : {a, b, b2, a, b2, a2})
                    p.vertices.push_back(v);
            }
            return true;
        }

        // A line pass made from its header at `head` and the packet's codes, the mark written at `markAt`.
        bool markLines(uint8_t *spr, uint32_t head, const uint8_t *codes, uint32_t bytes, uint32_t markAt)
        {
            float h[8][4];
            for (uint32_t k = 0; k < 8; ++k)
                std::memcpy(h[k], spr + ((head + 16u + 16u * k) & 0x3FF0u), 16);
            Pass &p = g_passes[g_nextMark % kPasses];
            if (!makeLines(codes, bytes, h, p))
            {
                g_leftWhy.insert(0x90000u); // a line pass not read
                ++g_left;
                return false;
            }
            p.mark = g_nextMark & 0xFFFFu;
            const uint32_t mark = 0x00A50000u | p.mark;
            std::memcpy(spr + (markAt & 0x3FFCu), &mark, 4);
            ++g_nextMark;
            ++g_made;
            return true;
        }

        // A line pass whose packet points at the vertices, under way: taken at the flush.
        struct PointerLines
        {
            uint32_t mesh = 0, p0 = 0;
        } g_pointerLines;

        void linePass(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            static const bool native = std::getenv("AMP_NATIVE") && std::atoi(std::getenv("AMP_NATIVE")) != 0;
            uint8_t *spr = rt->memory.scratchpad();
            uint32_t p0;
            std::memcpy(&p0, spr + 8, 4);
            const uint32_t self = ctx->r[4].uw[0], geometry = rd32(ram, self + 0x140);
            const bool pointer = rd32(ram, geometry + 0x154) != 0;
            if (native && pointer)
                g_pointerLines = {self, p0};
            func_0x2159b0(ram, ctx, rt);
            g_pointerLines = {};
            if (!native || pointer)
                return;
            uint32_t p1;
            std::memcpy(&p1, spr + 8, 4);
            if (p1 <= p0 || p1 - p0 > 0x1000u)
                return;
            auto word = [&](uint32_t a) {
                uint32_t v;
                std::memcpy(&v, spr + (a & 0x3FFCu), 4);
                return v;
            };
            uint32_t head = p0;
            if (word(head + 12) != 0x6c088000u)
                head += 16;
            if (word(head + 12) != 0x6c088000u || word(head) != 0 || head + 16u * 9u > p1)
            {
                g_leftWhy.insert(0x90001u); // no line header where expected
                ++g_left;
                return;
            }
            const uint32_t qwc = rd32(ram, geometry + 0x16e) & 0xFFFFu;
            uint32_t ref = 0;
            for (uint32_t a = head + 16u * 9u; a + 16 <= p1; a += 16)
                if (((word(a) >> 28) & 7u) == 3u && (word(a) & 0xFFFFu) == qwc)
                {
                    ref = a;
                    break;
                }
            const uint32_t packet = ref ? word(ref + 4) & 0x1FFFFF0u : 0;
            if (!ref || packet + 16u * qwc > 0x2000000u)
            {
                g_leftWhy.insert(0x90002u); // no REF of the line packet
                ++g_left;
                return;
            }
            if (!markLines(spr, head, ram + packet, 16u * qwc, head))
                return;
            const uint32_t emptied = word(ref) & ~0xFFFFu;
            std::memcpy(spr + (ref & 0x3FF0u), &emptied, 4);
        }

        // At the flush in a line pass whose packet points at the vertices (copied into the scratchpad just before):
        // the copy read, made one CNT of NOPs with the mark first.
        void pointerLinesFlush(const uint8_t *ram, uint8_t *spr)
        {
            const PointerLines pl = g_pointerLines;
            g_pointerLines = {};
            uint32_t end;
            std::memcpy(&end, spr + 8, 4);
            const uint32_t geometry = rd32(ram, pl.mesh + 0x140), qwc = rd32(ram, geometry + 0x16e) & 0xFFFFu;
            const uint32_t start = end - 16u * qwc;
            auto word = [&](uint32_t a) {
                uint32_t v;
                std::memcpy(&v, spr + (a & 0x3FFCu), 4);
                return v;
            };
            uint32_t head = pl.p0;
            if (word(head + 12) != 0x6c088000u)
                head += 16;
            static std::vector<uint8_t> codes;
            if (qwc < 2 || word(head + 12) != 0x6c088000u || (start & 0x3FF0u) + 16u * qwc > 0x4000u ||
                !gatherChain(ram, spr, start, qwc, codes))
            {
                g_leftWhy.insert(0x90003u); // a line packet pointing at the vertices not taken
                ++g_left;
                return;
            }
            float h[8][4];
            for (uint32_t k = 0; k < 8; ++k)
                std::memcpy(h[k], spr + ((head + 16u + 16u * k) & 0x3FF0u), 16);
            Pass &p = g_passes[g_nextMark % kPasses];
            if (!makeLines(codes.data(), static_cast<uint32_t>(codes.size()), h, p))
            {
                g_leftWhy.insert(0x90004u);
                ++g_left;
                return;
            }
            std::memset(spr + (start & 0x3FF0u), 0, 16u * qwc);
            const uint32_t cnt = 0x10000000u | (qwc - 1u);
            std::memcpy(spr + (start & 0x3FF0u), &cnt, 4);
            p.mark = g_nextMark & 0xFFFFu;
            const uint32_t mark = 0x00A50000u | p.mark;
            std::memcpy(spr + ((start + 16u) & 0x3FF0u), &mark, 4);
            ++g_nextMark;
            ++g_made;
        }

        // The flush of the scratchpad's packet to the ring (0x211128; it runs the VIF over it at once). In a pass whose
        // packet points at the vertices (0x2155b0 copies that packet into the scratchpad with channel 9, just before),
        // the pass is made native here: the mark before the header and the copied packet made one empty CNT.
        void ringFlush(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            if (g_multi.self)
                multiFlush(ram, rt->memory.scratchpad());
            else if (g_pointerLines.mesh)
                pointerLinesFlush(ram, rt->memory.scratchpad());
            else if (g_pointerPass.mesh)
            {
                const PointerPass pp = g_pointerPass;
                g_pointerPass = {};
                uint8_t *spr = rt->memory.scratchpad();
                uint32_t end;
                std::memcpy(&end, spr + 8, 4);
                const uint32_t geometry = rd32(ram, pp.mesh + 0x140), qwc = rd32(ram, geometry + 0x16c) & 0xFFFFu;
                const uint32_t start = end - 16u * qwc;
                auto word = [&](uint32_t a) {
                    uint32_t v;
                    std::memcpy(&v, spr + (a & 0x3FFCu), 4);
                    return v;
                };
                uint32_t head = pp.head;
                if (word(head + 12) != 0x6c158000u)
                    head += 16;
                // The packet's place in the scratchpad (the flush before the copy, 0x211100, may have sent the header
                // already: the mark goes into the emptied packet, a CNT of NOPs, not before the header).
                static std::vector<std::array<float, 16>> raw;
                if (qwc >= 2 && word(head + 12) == 0x6c158000u && (start & 0x3FF0u) + 16u * qwc <= 0x4000u &&
                    decodeChain(ram, spr, start, qwc, raw))
                {
                    std::memset(spr + (start & 0x3FF0u), 0, 16u * qwc);
                    const uint32_t cnt = 0x10000000u | (qwc - 1u);
                    std::memcpy(spr + (start & 0x3FF0u), &cnt, 4);
                    if (!markPass(spr, head, raw, start + 16u))
                        g_leftWhy.insert(0x10001u); // emptied but not made: not drawn
                }
                else
                {
                    g_leftWhy.insert(0x10000u); // a packet pointing at the vertices not taken
                    ++g_left;
                    // AMP_TRACE_POINTER=1: why (the first few)
                    static const bool trace = std::getenv("AMP_TRACE_POINTER") != nullptr;
                    static int shown = 0;
                    if (trace && ++shown <= 8)
                    {
                        std::printf("[amp:pointer] mesh %08x geometry %08x head %08x (%08x %08x) end %08x qwc %x start %08x "
                                    "vertices %08x-%08x faces %08x-%08x triangles %d\n",
                                    pp.mesh, geometry, head, word(head), word(head + 12), end, qwc, start,
                                    rd32(ram, geometry + 0xe8), rd32(ram, geometry + 0xec), rd32(ram, geometry + 0xf8),
                                    rd32(ram, geometry + 0xfc), int(decodeChain(ram, spr, start, qwc, raw)));
                        for (uint32_t a = pp.head; a < pp.head + 0x40; a += 16)
                            std::printf("  %04x: %08x %08x %08x %08x\n", a & 0x3FFFu, word(a), word(a + 4), word(a + 8), word(a + 12));
                        for (uint32_t a = start; a < start + 0x30; a += 16)
                            std::printf("  %04x: %08x %08x %08x %08x\n", a & 0x3FFFu, word(a), word(a + 4), word(a + 8), word(a + 12));
                    }
                }
            }
            func_0x211128(ram, ctx, rt);
        }
    } // namespace

    void native::pass(uint32_t mark)
    {
        Pass &p = g_passes[mark % kPasses];
        if (p.mark != mark)
        {
            ++g_missed;
            return;
        }
        gs::nativeTriangles(p.prim, p.matrix, p.vertices.data(), static_cast<uint32_t>(p.vertices.size()));
        p.mark = ~0u;
        ++g_drawn;
    }

    void native::report()
    {
        if (!g_made && !g_left)
            return;
        std::printf("[amp:native] passes made %llu, drawn %llu, marks missed %llu, left to VU1 %llu (", (unsigned long long)g_made,
                    (unsigned long long)g_drawn, (unsigned long long)g_missed, (unsigned long long)g_left);
        for (const uint32_t why : g_leftWhy)
            std::printf(" %x", why);
        std::printf(" )\n");
    }

    void installNative(Runtime &, bozo::port::Registry &r)
    {
        r.registerFunction(0x215f30, meshDraw); // the PS2 mesh's draw
        r.registerFunction(0x2155b0, meshPass); // a mesh pass's VU1 header and packet
        r.registerFunction(0x2159b0, linePass); // a mesh's line pass (VU1 program 0x5a8)
        r.registerFunction(0x211128, ringFlush); // the scratchpad's packet to the ring
        r.registerFunction(0x21a4d0, multiDraw); // a PsMultiMesh's draw
        r.registerFunction(0x212500, sprTransfer); // a transfer to the scratchpad started
    }
} // namespace amp
