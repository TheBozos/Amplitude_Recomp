// Amplitude's native renderer (in progress): the VU1 programs Harmonix's renderer runs answered natively, their meshes,
// lines, multi-meshes and particles drawn through BozoEngine's 3D path with their own matrices, instead of VU1's
// microcode and the GIF packets it kicks. docs/RECOMP_NOTES.md ("Native renderer") maps the classes, functions and
// programs.
//
// Where: at the program's start (MSCAL, MSCNT: runtime/amp_vu1.cpp's startProgram). The VIF has unpacked the batch into
// VU1's data memory by then, so each program here reads its header and batch where its microcode would (from TOPS),
// draws, and leaves the unit's continuation where the microcode's end would (MSCNT goes on from there). A program's
// header is kept from its MSCAL for the MSCNT batches after it, as the microcode keeps it in its registers. A program
// start not answered here (an unknown lighting sub-program, a header not read) runs on VU1 as before, and the MSCNTs
// after it too. Answering at the start rather than at the game's functions keeps it whole when a function's chain is
// cut by a flush, sent in pieces, or when guest code leaves an override midway and goes on outside it.
//
// The programs (their microcode in tools/amp_vu1_recomp.py's images):
//   0x31f0 (MSCAL 0x63e, MSCNT 0x3a78) image 8: a mesh pass (0x2155b0, the mesh's draw 0x215f30). Header 21 quadwords:
//     0 vf19 the clipping guard   1-4 vf20-23 the clip matrix (columns)   5 vf13 the colour's scale   6, 7 vf14 / vf15:
//     screen = vf14 * clip / w + vf15 (x, y, z), fog = vf14.w * w + vf15.w   8-10 vf16-18 the texture's transform
//     11 the GIF tag (PRE: PRIM)   12 the clipped fans' tag   13 vi13 the colour sub-program (0x260 three lights, 0x373
//     the ambient, 0x3d7 none, 0x6c a point light), vi14 / vi15 (the vertex colour times the ambient, the lights)
//     14 vf25 the ambient   15-17 vf26-28 the lights' directions   18-20 vf29-31 their colours.
//     A batch: the counts at 21 (vertices, faces), the vertices from 22 (4 quadwords: the position, the normal, the
//     colour 0-1, u v), the faces after them (each vertex's quadword offset).
//   0x2d40 (MSCAL 0x5a8, MSCNT 0x2ff0): a mesh's line pass (0x2159b0). Header 8: vf28-31 the clip matrix, the tag,
//     vf26 / vf27 the viewport (w: the fog's terms), vf25 the lines' colour (integers). A batch: the counts at 8
//     (vertices, lines), the positions from 9 (2 quadwords a vertex), the lines after them (each end's offset).
//   0x1ee0 (MSCAL 0x3dc, MSCNT 0x2340): a PsMultiMesh (0x21a4d0): one mesh at each instance. Header 19: vf16-19 the
//     clip matrix, vf23 the colour's scale, vf24 / vf15 the viewport, vf20-22 the texture's transform, the tag, vi12 /
//     vi14 the colour sub-program and its switch, vf25-31 the lights. The mesh's batch at 21 (as image 8's), lit and
//     mapped once at the MSCAL (the MSCNT copies it into its buffer); after it the instance block: a count, then each
//     instance's x, y, z axes and position (4 quadwords).
//   0x2890 (MSCAL 0x512, MSCNT 0x2a78): a particle system (0x219f28): each particle a SPRITE. Header 9: vf28-31 the clip
//     matrix, vf25 the colour's scale, the tag, vf26 / vf27 the viewport, vf24 the corners' offset in clip space. A
//     batch: the count at 9, then each particle's colour (floats) and position (w half its size) from 10.
#include "amp_functions.h"
#include "amp_gs.h"
#include "amp_port.h"
#include "bozo/render/renderer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace amp
{
    namespace
    {
        using bozo::render::Vertex;

        uint64_t g_batches = 0, g_vu1 = 0; // AMP_NATIVE's report: batches drawn here, program starts left to VU1
        std::map<uint32_t, uint64_t> g_leftWhy; // why (a colour sub-program, or a program's code), how often

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

        // ---- The programs' batches, read from VU1's data memory -----------------------------------------------------

        // VU1's data memory from a program's TOPS: quadword `a` (wrapping at 1024), as floats or words.
        struct Memory
        {
            const uint8_t *data;
            uint32_t top;
            const float *f(uint32_t a) const { return reinterpret_cast<const float *>(data + 16u * ((top + a) & 0x3FFu)); }
            const uint32_t *u(uint32_t a) const { return reinterpret_cast<const uint32_t *>(data + 16u * ((top + a) & 0x3FFu)); }
        };

        void draw(uint64_t prim, const float matrix[16], const std::vector<Vertex> &v)
        {
            if (!v.empty())
                gs::nativeTriangles(prim, matrix, v.data(), static_cast<uint32_t>(v.size()));
            ++g_batches;
        }

        const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

        // A triangle batch's vertices (image 8's, a multi-mesh's mesh): the counts at `at` (vertices, faces), the
        // vertices after them (4 quadwords each), the faces after those (each vertex's quadword offset, x y z). Each
        // face's three vertices (16 floats) handed to `each`; false for counts or offsets out of the memory.
        template <class Each> bool triangleBatch(const Memory &m, uint32_t at, Each &&each)
        {
            const uint32_t vertices = m.u(at)[0], faces = m.u(at)[1];
            if (vertices > 250u || at + 1u + 4u * vertices + faces > 1024u)
                return false;
            for (uint32_t f = 0; f < faces; ++f)
                for (uint32_t k = 0; k < 3; ++k)
                {
                    const uint32_t offset = m.u(at + 1u + 4u * vertices + f)[k] & 0xFFFFu;
                    if (offset / 4u >= vertices)
                        return false;
                    each(m.f(at + 1u + offset));
                }
            return true;
        }

        // ---- 0x31f0 / 0x3a78: image 8, a mesh pass -----------------------------------------------------------------
        struct Image8
        {
            bool on = false;
            float h[21][4];
            Shading sh;
            uint64_t prim = 0;
            float matrix[16];
        } g_image8;

        bool image8Start(const Memory &m)
        {
            Image8 &p = g_image8;
            p.on = false;
            for (uint32_t k = 0; k < 21; ++k)
                std::memcpy(p.h[k], m.f(k), 16);
            const float (*h)[4] = p.h;
            uint32_t sub, vi14, vi15;
            std::memcpy(&sub, h[13], 4);
            std::memcpy(&vi14, &h[13][1], 4);
            std::memcpy(&vi15, &h[13][2], 4);
            p.sh = Shading{sub & 0xFFFFu, (vi14 & 0xFFFFu) != 0, (vi15 & 0xFFFFu) != 0, h[14], h[15], h[16], h[17],
                           h[18], h[19], h[20], h[5], h[8], h[9], h[10]};
            if (!p.sh.known())
                return ++g_leftWhy[p.sh.sub], false;
            if (!tagPrim(h[11], p.prim))
                return ++g_leftWhy[0x31f0001u], false;
            toScreen(&h[1], h[6], h[7], p.matrix);
            return p.on = true;
        }

        bool image8Batch(const Memory &m)
        {
            const Image8 &p = g_image8;
            const float (*h)[4] = p.h;
            static std::vector<Vertex> tris;
            tris.clear();
            const bool ok = triangleBatch(m, 21, [&](const float *q) {
                Vertex v;
                shade(p.sh, q, v);
                // The fog: vf14.w * clip.w + vf15.w, 0-255 (not over w).
                const float w = h[1][3] * v.x + h[2][3] * v.y + h[3][3] * v.z + h[4][3];
                v.fog = float(static_cast<uint32_t>(std::clamp(h[6][3] * w + h[7][3], 0.0f, 255.0f))) * (1.0f / 255.0f);
                tris.push_back(v);
            });
            if (!ok)
                return false;
            clipDepth(&h[1], tris);
            draw(p.prim, p.matrix, tris);
            return true;
        }

        // ---- 0x2d40 / 0x2ff0: a mesh's lines ----------------------------------------------------------------------
        struct Lines
        {
            bool on = false;
            float h[8][4];
            uint64_t prim = 0;
            float m[16];
            uint32_t rgba = 0;
        } g_lines;

        bool linesStart(const Memory &m)
        {
            Lines &p = g_lines;
            p.on = false;
            for (uint32_t k = 0; k < 8; ++k)
                std::memcpy(p.h[k], m.f(k), 16);
            if (!tagPrim(p.h[4], p.prim))
                return ++g_leftWhy[0x2d40001u], false;
            p.prim = (p.prim & ~7ull) | 3u; // drawn as triangles: each line a quad, as the GS path draws a LINE
            toScreen(&p.h[0], p.h[5], p.h[6], p.m);
            uint32_t c[4];
            std::memcpy(c, p.h[7], 16);
            p.rgba = (c[0] & 0xFFu) | (c[1] & 0xFFu) << 8 | (c[2] & 0xFFu) << 16 | (c[3] & 0xFFu) << 24;
            return p.on = true;
        }

        // The program does not clip a line: it drops it when either end's clip flags are set (that end's test against
        // -w..w on x, y, z and the vertex's before it, as FCGET gives them), and drops the batch when every vertex's
        // flags share a bit. Each line kept is a quad one GS pixel tall in GS coordinates (its ends are inside: w > 0).
        bool linesBatch(const Memory &m)
        {
            const Lines &p = g_lines;
            const float (*h)[4] = p.h;
            const uint32_t vertices = m.u(8)[0], lines = m.u(8)[1];
            if (vertices > 500u || 9u + 2u * vertices + lines > 1024u)
                return false;
            static std::vector<uint32_t> flags;
            flags.resize(vertices);
            uint32_t previous = 0, all = 0xFFFu;
            for (uint32_t k = 0; k < vertices; ++k)
            {
                const float *q = m.f(9u + 2u * k);
                float c[4];
                for (int r = 0; r < 4; ++r)
                    c[r] = h[0][r] * q[0] + h[1][r] * q[1] + h[2][r] * q[2] + h[3][r];
                const float w = std::fabs(c[3]);
                const uint32_t test = (c[0] > w ? 1u : 0u) | (c[0] < -w ? 2u : 0u) | (c[1] > w ? 4u : 0u) |
                                      (c[1] < -w ? 8u : 0u) | (c[2] > w ? 16u : 0u) | (c[2] < -w ? 32u : 0u);
                flags[k] = test | (previous << 6);
                previous = test;
                all &= flags[k];
            }
            static std::vector<Vertex> quads;
            quads.clear();
            auto end = [&](const float *q) {
                Vertex v;
                const float *mm = p.m;
                const float x = q[0], y = q[1], z = q[2];
                const float w = mm[3] * x + mm[7] * y + mm[11] * z + mm[15];
                v.x = (mm[0] * x + mm[4] * y + mm[8] * z + mm[12]) / w;
                v.y = (mm[1] * x + mm[5] * y + mm[9] * z + mm[13]) / w;
                v.z = (mm[2] * x + mm[6] * y + mm[10] * z + mm[14]) / w;
                v.rgba = p.rgba;
                v.u = v.v = 0.0f;
                v.q = 1.0f;
                const float cw = h[0][3] * x + h[1][3] * y + h[2][3] * z + h[3][3];
                v.fog = float(static_cast<uint32_t>(std::clamp(h[5][3] * cw + h[6][3], 0.0f, 255.0f))) * (1.0f / 255.0f);
                return v;
            };
            if (!all)
                for (uint32_t l = 0; l < lines; ++l)
                {
                    const uint32_t a = (m.u(9u + 2u * vertices + l)[0] & 0xFFFFu) / 2u;
                    const uint32_t b = (m.u(9u + 2u * vertices + l)[1] & 0xFFFFu) / 2u;
                    if (a >= vertices || b >= vertices)
                        return false;
                    if (flags[a] | flags[b])
                        continue;
                    const Vertex va = end(m.f(9u + 2u * a)), vb = end(m.f(9u + 2u * b));
                    Vertex a2 = va, b2 = vb;
                    a2.y += 1.0f;
                    b2.y += 1.0f;
                    for (const Vertex &v : {va, vb, b2, va, b2, a2})
                        quads.push_back(v);
                }
            draw(p.prim, kIdentity, quads);
            return true;
        }

        // ---- 0x1ee0 / 0x2340: a multi-mesh -------------------------------------------------------------------------
        struct Multi
        {
            bool on = false;
            float h[19][4];
            uint64_t prim = 0;
            float matrix[16];
            std::vector<Vertex> tris; // the mesh's triangles, lit and mapped, in its own space
            uint32_t instances = 0;   // where the instance block is (from TOPS)
        } g_multi;

        bool multiStart(const Memory &m)
        {
            Multi &p = g_multi;
            p.on = false;
            for (uint32_t k = 0; k < 19; ++k)
                std::memcpy(p.h[k], m.f(k), 16);
            const float (*h)[4] = p.h;
            uint32_t sub, vi14;
            std::memcpy(&sub, h[11], 4);
            std::memcpy(&vi14, &h[11][1], 4);
            // vi15 is not in this header (the unit keeps its own): taken as clear, as image 8 leaves it.
            const Shading sh{sub & 0xFFFFu, (vi14 & 0xFFFFu) != 0, false, h[12], h[13], h[14], h[15],
                             h[16], h[17], h[18], h[4], h[7], h[8], h[9]};
            if (!sh.known())
                return ++g_leftWhy[sh.sub | 0x1000000u], false;
            if (!tagPrim(h[10], p.prim))
                return ++g_leftWhy[0x1ee0001u], false;
            toScreen(&h[0], h[5], h[6], p.matrix);
            p.tris.clear();
            const bool ok = triangleBatch(m, 21, [&](const float *q) {
                Vertex v;
                shade(sh, q, v);
                v.fog = 0.0f; // the program leaves F 0
                p.tris.push_back(v);
            });
            if (!ok)
                return ++g_leftWhy[0x1ee0002u], false;
            p.instances = 22u + 4u * m.u(21)[0] + m.u(21)[1];
            return p.on = true;
        }

        // The instance block at the batch's TOPS: a count, then each instance's x, y, z axes and position.
        bool multiBatch(const Memory &m)
        {
            const Multi &p = g_multi;
            const uint32_t count = m.u(p.instances)[0];
            if (count > 255u || p.instances + 1u + 4u * count > 1024u)
                return false;
            static std::vector<Vertex> all;
            all.clear();
            for (uint32_t i = 0; i < count; ++i)
            {
                float t[16];
                for (uint32_t k = 0; k < 4; ++k)
                    std::memcpy(t + 4 * k, m.f(p.instances + 1u + 4u * i + k), 16);
                for (const Vertex &v : p.tris)
                {
                    Vertex w = v;
                    w.x = t[0] * v.x + t[4] * v.y + t[8] * v.z + t[12];
                    w.y = t[1] * v.x + t[5] * v.y + t[9] * v.z + t[13];
                    w.z = t[2] * v.x + t[6] * v.y + t[10] * v.z + t[14];
                    all.push_back(w);
                }
            }
            clipDepth(&p.h[0], all);
            draw(p.prim, p.matrix, all);
            return true;
        }

        // ---- 0x2890 / 0x2a78: particles ----------------------------------------------------------------------------
        struct Particles
        {
            bool on = false;
            float h[9][4];
            uint64_t prim = 0;
        } g_particles;

        bool particlesStart(const Memory &m)
        {
            Particles &p = g_particles;
            p.on = false;
            for (uint32_t k = 0; k < 9; ++k)
                std::memcpy(p.h[k], m.f(k), 16);
            if (!tagPrim(p.h[5], p.prim))
                return ++g_leftWhy[0x2890001u], false;
            p.prim = (p.prim & ~7ull) | 3u; // drawn as two triangles, as the GS path draws a SPRITE
            return p.on = true;
        }

        // Each particle's corners are the centre in clip space less and plus vf24 times its half size, both projected
        // with the first corner's 1 / w; a particle with either corner outside the clip space is dropped; u v 0 to 1
        // across (the sprite's Q is the second corner's), F 0.
        bool particlesBatch(const Memory &m)
        {
            const Particles &p = g_particles;
            const float (*h)[4] = p.h;
            const uint32_t count = m.u(9)[0];
            if (count > 500u || 10u + 2u * count > 1024u)
                return false;
            static std::vector<Vertex> sprites;
            sprites.clear();
            for (uint32_t i = 0; i < count; ++i)
            {
                const float *colour = m.f(10u + 2u * i), *position = m.f(11u + 2u * i);
                float c[4], a[4], b[4];
                for (int r = 0; r < 4; ++r)
                    c[r] = h[0][r] * position[0] + h[1][r] * position[1] + h[2][r] * position[2] + h[3][r];
                for (int r = 0; r < 4; ++r)
                {
                    a[r] = c[r] - h[8][r] * position[3];
                    b[r] = c[r] + h[8][r] * position[3];
                }
                bool outside = false;
                for (const float *k : {a, b})
                {
                    const float w = std::fabs(k[3]);
                    outside |= k[0] > w || k[0] < -w || k[1] > w || k[1] < -w || k[2] > w || k[2] < -w;
                }
                if (outside || a[3] == 0.0f)
                    continue;
                const float qa = 1.0f / a[3];
                uint32_t rgba = 0;
                for (int k = 0; k < 4; ++k)
                {
                    const float x = h[4][k] * colour[k]; // ftoi0
                    rgba |= (x <= 0.0f ? 0u : x >= 255.0f ? 255u : static_cast<uint32_t>(x)) << (8 * k);
                }
                auto corner = [&](const float *k, float uv) {
                    Vertex v;
                    v.x = (h[6][0] * k[0] + h[7][0] * k[3]) * qa;
                    v.y = (h[6][1] * k[1] + h[7][1] * k[3]) * qa;
                    v.z = (h[6][2] * k[2] + h[7][2] * k[3]) * qa;
                    v.u = v.v = uv;
                    v.q = 1.0f;
                    v.rgba = rgba;
                    v.fog = 0.0f;
                    return v;
                };
                const Vertex va = corner(a, 0.0f), vb = corner(b, 1.0f);
                Vertex tr = va, bl = va;
                tr.x = vb.x;
                tr.u = vb.u;
                bl.y = vb.y;
                bl.v = vb.v;
                for (const Vertex &v : {va, tr, vb, va, vb, bl})
                    sprites.push_back(v);
            }
            draw(p.prim, kIdentity, sprites);
            return true;
        }
    } // namespace

    bool native::program(uint32_t pcBytes, const uint8_t *vuData, uint32_t top, uint32_t &tpc)
    {
        static const bool on = std::getenv("AMP_NATIVE") && std::atoi(std::getenv("AMP_NATIVE")) != 0;
        if (!on)
            return false;
        const Memory m{vuData, top};
        // A start answered here leaves the continuation where the program's end would (its [E] pair, plus two). An
        // MSCAL whose header or batch is not read runs on VU1, and the MSCNTs after it; an MSCNT after an MSCAL
        // answered here has to be answered here too (the unit's registers do not hold the header): one not read is lost.
        auto handled = [&](uint32_t next) {
            tpc = next;
            return true;
        };
        auto toVu1 = [&]() {
            ++g_vu1;
            return false;
        };
        auto lost = [&]() { ++g_leftWhy[pcBytes << 4]; };
        switch (pcBytes)
        {
        case 0x31f0:
            if (image8Start(m) && image8Batch(m))
                return handled(0x3a78);
            g_image8.on = false;
            return toVu1();
        case 0x3a78:
            if (!g_image8.on)
                return toVu1();
            if (!image8Batch(m))
                lost();
            return handled(0x3a78);
        case 0x2d40:
            if (linesStart(m) && linesBatch(m))
                return handled(0x2ff0);
            g_lines.on = false;
            return toVu1();
        case 0x2ff0:
            if (!g_lines.on)
                return toVu1();
            if (!linesBatch(m))
                lost();
            return handled(0x2ff0);
        case 0x1ee0:
            if (multiStart(m) && multiBatch(m))
                return handled(0x2340);
            g_multi.on = false;
            return toVu1();
        case 0x2340:
            if (!g_multi.on)
                return toVu1();
            if (!multiBatch(m))
                lost();
            return handled(0x2340);
        case 0x2890:
            if (particlesStart(m) && particlesBatch(m))
                return handled(0x2a78);
            g_particles.on = false;
            return toVu1();
        case 0x2a78:
            if (!g_particles.on)
                return toVu1();
            if (!particlesBatch(m))
                lost();
            return handled(0x2a78);
        default: ++g_vu1; return false;
        }
    }

    void native::report()
    {
        if (!g_batches && !g_vu1)
            return;
        std::printf("[amp:native] batches drawn %llu, program starts left to VU1 %llu (", (unsigned long long)g_batches,
                    (unsigned long long)g_vu1);
        for (const auto &[why, count] : g_leftWhy)
            std::printf(" %x:%llu", why, (unsigned long long)count);
        std::printf(" )\n");
    }

    void installNative(Runtime &, bozo::port::Registry &r)
    {
        r.registerFunction(0x215f30, meshDraw); // the PS2 mesh's draw (AMP_TRACE_MESHES)
    }
} // namespace amp
