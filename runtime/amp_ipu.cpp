// Amplitude's IPU movies (the animated textures: metagame/image/*.ipu, the arenas' and songs' movieN.ipu), decoded natively.
//
// The game's "Movie" class (registered by 0x2163c0, which also installs its DMA channel 3 handler 0x216340) plays
// `ipum` streams (a 16-byte header: "ipum", the size, width, height, the frame count; then each picture: a flag byte,
// MPEG intra macroblocks, the end code 0x000001b0) on the IPU, several streams taking turns:
//   - 0x216a18 (a picture's start): the first time, the to-IPU DMA chain over the stream (0x21b520: D4 chain mode);
//     else the stream's DMA state restored (0x30ed90, libipu's restart, from this + 0x58). The texture's buffer (its
//     source object's virtual call, +8) becomes the from-IPU destination (D3_MADR, D3_QWC = macroblocks x 32: RGB16);
//     0x21b560 reads the picture's flag byte (FDEC 0, then FDEC 8) and the IDEC command decodes it (CSC, RGB16 out).
//   - the from-IPU DMA's end (channel 3's interrupt, 0x216340 -> 0x216990): the picture is handed to the texture, the
//     end code skipped (FDEC 32), the stream's DMA state saved (0x30eca8, libipu's stop, to this + 0x58), and the next
//     waiting movie's picture started.
// Natively: the to-IPU chain is read once into a byte stream, the stream follows its owner through the save / restore
// calls, the flag peek reads it, and IDEC decodes the picture (the flag byte to the end code) with FFmpeg's IPU decoder,
// converts it with the IPU's colour conversion to RGB16 in macroblock order, as the IPU writes it, at D3_MADR, then
// raises the channel 3 interrupt. The other IPU and DMA register accesses of the class are native sites (nothing to do).
#include "amp_port.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/ee/kernel.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace amp
{
    bozo::libs::Libraries *libraries(); // runtime/amp_audio.cpp

    namespace
    {
        uint8_t *g_ram = nullptr;
        uint32_t rd32(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x1FFFFFFu), 4); return v; }
        uint64_t rd64(uint32_t a) { uint64_t v; std::memcpy(&v, g_ram + (a & 0x1FFFFFFu), 8); return v; }
        bool trace() { static const bool on = std::getenv("AMP_TRACE_IPU") != nullptr; return on; }

        struct Stream
        {
            uint32_t chain = 0;
            std::vector<uint8_t> bytes;
            int width = 0, height = 0; // from the stream's ipum header
            size_t pos = 0;      // the next byte the IPU would read
            size_t picture = 0;  // where the picture being decoded starts (its flag byte)
            uint64_t pictures = 0;
        };
        std::map<uint32_t, std::unique_ptr<Stream>> g_streams; // by their to-IPU chain
        std::map<uint32_t, Stream *> g_saved;                  // by the DMA state area the stop call saved them to
        Stream *g_current = nullptr;
        uint32_t g_d3madr = 0, g_d3qwc = 0;
        uint64_t g_decoded = 0, g_failed = 0;

        struct Decoder
        {
            AVCodecContext *context = nullptr;
            AVPacket *packet = nullptr;
            AVFrame *frame = nullptr;
            Decoder(int width, int height)
            {
                // FFmpeg's IPU decoder takes the picture size from its context (its demuxer reads the ipum header).
                const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_IPU);
                context = codec ? avcodec_alloc_context3(codec) : nullptr;
                if (context)
                {
                    context->width = width;
                    context->height = height;
                }
                if (!context || avcodec_open2(context, codec, nullptr) < 0)
                {
                    std::fprintf(stderr, "[amp:ipu] FFmpeg has no IPU decoder\n");
                    if (context)
                        avcodec_free_context(&context);
                }
                packet = av_packet_alloc();
                frame = av_frame_alloc();
            }
        };

        // The to-IPU chain (REF tags over the stream; refe, cnt and end as the DMA takes them): its data, in order.
        std::vector<uint8_t> readChain(uint32_t tadr)
        {
            std::vector<uint8_t> out;
            for (int guard = 0; guard < 100000; ++guard)
            {
                const uint64_t tag = rd64(tadr);
                const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu), id = static_cast<uint32_t>(tag >> 28) & 7u;
                const uint32_t addr = static_cast<uint32_t>(tag >> 32) & 0x1FFFFFF0u;
                const uint32_t from = (id == 1 || id == 2 || id == 7) ? tadr + 16 : addr;
                const uint8_t *p = g_ram + (from & 0x1FFFFFFu);
                out.insert(out.end(), p, p + qwc * 16);
                if (id == 0 || id == 7)
                    break;
                tadr = id == 1 ? tadr + 16 + qwc * 16 : id == 2 ? addr : tadr + 16;
            }
            return out;
        }

        // The IPU's colour conversion (YCbCr to RGB): BT.601 studio range, as Onimusha_Recomp's oni_bg.cpp has it.
        void csc(int y, int cb, int cr, int &r, int &g, int &b)
        {
            const int luma = 149 * std::max(0, y - 16);
            cb -= 128;
            cr -= 128;
            r = std::clamp((luma + 204 * cr + 64) >> 7, 0, 255);
            g = std::clamp((luma - 50 * cb - 104 * cr + 64) >> 7, 0, 255);
            b = std::clamp((luma + 258 * cb + 64) >> 7, 0, 255);
        }

        // IDEC: the current stream's picture (from its flag byte to the end code) decoded to RGB16 (A at bit 15,
        // opaque) in macroblock order at D3_MADR, as many macroblocks as D3_QWC holds (32 quadwords each).
        void decodePicture()
        {
            static std::map<std::pair<int, int>, std::unique_ptr<Decoder>> decoders; // one a picture size
            Stream *s = g_current;
            if (!s || !s->width || !s->height)
                return;
            auto &d = decoders[{s->width, s->height}];
            if (!d)
                d = std::make_unique<Decoder>(s->width, s->height);
            Decoder &decoder = *d;
            if (!decoder.context)
                return;
            size_t end = s->bytes.size();
            for (size_t at = s->picture + 1; at + 4 <= s->bytes.size(); ++at)
                if (s->bytes[at] == 0 && s->bytes[at + 1] == 0 && s->bytes[at + 2] == 1 && s->bytes[at + 3] == 0xB0)
                {
                    end = at + 4;
                    break;
                }
            std::vector<uint8_t> data(s->bytes.begin() + static_cast<long>(s->picture), s->bytes.begin() + static_cast<long>(end));
            const int bytes = static_cast<int>(data.size());
            data.resize(data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
            av_packet_unref(decoder.packet);
            decoder.packet->data = data.data();
            decoder.packet->size = bytes;
            int got = avcodec_send_packet(decoder.context, decoder.packet);
            if (got >= 0)
                got = avcodec_receive_frame(decoder.context, decoder.frame);
            decoder.packet->data = nullptr;
            decoder.packet->size = 0;
            s->pos = end; // the IDEC read up to the end code (the handler's FDEC 32 skips it: native, nothing to do)
            ++s->pictures;
            if (got < 0)
            {
                ++g_failed;
                if (g_failed < 8)
                    std::fprintf(stderr, "[amp:ipu] picture at %zu of stream %x: decode failed (%d)\n", s->picture, s->chain, got);
                avcodec_flush_buffers(decoder.context);
                return;
            }
            const AVFrame *f = decoder.frame;
            const uint32_t mbs = g_d3qwc / 32u, columns = static_cast<uint32_t>(f->width + 15) / 16u;
            uint8_t *out = g_ram + (g_d3madr & 0x1FFFFFFu);
            for (uint32_t mb = 0; mb < mbs; ++mb)
            {
                const int mx = static_cast<int>(mb % columns) * 16, my = static_cast<int>(mb / columns) * 16;
                for (int y = 0; y < 16; ++y)
                    for (int x = 0; x < 16; ++x)
                    {
                        const int px = std::min(mx + x, f->width - 1), py = std::min(my + y, f->height - 1);
                        int r, g, b;
                        csc(f->data[0][py * f->linesize[0] + px], f->data[1][(py / 2) * f->linesize[1] + px / 2],
                            f->data[2][(py / 2) * f->linesize[2] + px / 2], r, g, b);
                        const uint16_t c = static_cast<uint16_t>((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10 | 0x8000);
                        std::memcpy(out + (mb * 256 + static_cast<uint32_t>(y) * 16 + static_cast<uint32_t>(x)) * 2, &c, 2);
                    }
            }
            av_frame_unref(decoder.frame);
            ++g_decoded;
            if (trace())
                std::printf("[amp:ipu] stream %x picture %llu (%zu bytes) %dx%d -> %x (%u macroblocks)\n", s->chain,
                            (unsigned long long)s->pictures, data.size() - AV_INPUT_BUFFER_PADDING_SIZE, f->width, f->height,
                            g_d3madr, mbs);
        }

        void returnTo(R5900Context *ctx, int64_t v)
        {
            ctx->r[2].sd[0] = v;
            ctx->pc = ctx->r[31].uw[0];
        }

        // 0x21b520(chain): the to-IPU DMA started in chain mode over a stream: read it whole (once per chain).
        void startStream(uint8_t *, R5900Context *ctx, Runtime *)
        {
            const uint32_t chain = ctx->r[4].uw[0] & 0x0FFFFFFFu;
            auto &s = g_streams[chain];
            if (!s)
            {
                s = std::make_unique<Stream>();
                s->chain = chain;
            }
            s->bytes = readChain(chain);
            s->pos = 0;
            // The ipum header (16 bytes: "ipum", the size, the width and height as halfwords, the frame count): at the
            // stream's start, or just before the chain's first data.
            const uint8_t *header = nullptr;
            if (s->bytes.size() >= 16 && std::memcmp(s->bytes.data(), "ipum", 4) == 0)
                header = s->bytes.data();
            else
            {
                const uint64_t tag = rd64(chain);
                const uint32_t id = static_cast<uint32_t>(tag >> 28) & 7u;
                const uint32_t data = (id == 1 || id == 2 || id == 7) ? chain + 16 : static_cast<uint32_t>(tag >> 32) & 0x1FFFFFF0u;
                if (data >= 16 && std::memcmp(g_ram + ((data - 16) & 0x1FFFFFFu), "ipum", 4) == 0)
                    header = g_ram + ((data - 16) & 0x1FFFFFFu);
            }
            if (header)
            {
                s->width = header[8] | header[9] << 8;
                s->height = header[10] | header[11] << 8;
                if (header == s->bytes.data())
                    s->bytes.erase(s->bytes.begin(), s->bytes.begin() + 16);
            }
            g_current = s.get();
            if (trace())
                std::printf("[amp:ipu] stream %x: %zu bytes, %dx%d%s\n", chain, s->bytes.size(), s->width, s->height,
                            header ? "" : " (no ipum header found)");
            returnTo(ctx, 0);
        }

        // 0x30eca8(state) / 0x30ed90(state): libipu's stop and restart of the IPU's DMA, its state (and the IPU's input
        // position) kept at `state`: the current stream follows.
        void stopDma(uint8_t *, R5900Context *ctx, Runtime *)
        {
            g_saved[ctx->r[4].uw[0]] = g_current;
            returnTo(ctx, 0);
        }
        void restartDma(uint8_t *, R5900Context *ctx, Runtime *)
        {
            if (auto it = g_saved.find(ctx->r[4].uw[0]); it != g_saved.end() && it->second)
                g_current = it->second;
            returnTo(ctx, 0);
        }

        // 0x21b560(): the picture's flag byte (FDEC 0: the next 32 bits, their top byte), then FDEC 8 past it. Past the
        // stream's end, its first picture again.
        void pictureFlags(uint8_t *, R5900Context *ctx, Runtime *)
        {
            Stream *s = g_current;
            uint32_t flags = 0;
            if (s && !s->bytes.empty())
            {
                if (s->pos >= s->bytes.size())
                    s->pos = 0;
                s->picture = s->pos;
                flags = s->bytes[s->pos++];
            }
            returnTo(ctx, flags);
        }

        // sceIpuSync(mode, timeout) (0x30eee0): the IPU is never busy.
        void ipuSync(uint8_t *, R5900Context *ctx, Runtime *) { returnTo(ctx, 0); }
    } // namespace

    // The Movie class's IPU and DMA register accesses (native sites).
    bool ipuSite(uint8_t *, R5900Context *ctx, Runtime *, uint32_t pc)
    {
        switch (pc)
        {
        case 0x216a44u: // native site: D_ENABLEW (DMA held)
        case 0x216a4cu: // native site: D4_CHCR (to-IPU stopped)
        case 0x216a54u: // native site: D_ENABLEW (released)
        case 0x216a68u: // native site: IPU_CTRL (reset)
        case 0x216a84u: // native site: IPU_CMD (BCLR)
        case 0x216a98u: // native site: IPU_CTRL
        case 0x216b04u: // native site: D3_CHCR (from-IPU started)
        case 0x216b3cu: // native site: IPU_CTRL (the picture's flags)
        case 0x2169d4u: // native site: IPU_CMD (FDEC 32: the end code, consumed by the IDEC)
            return true;
        case 0x216ae8u: // native site: D3_MADR (the texture's buffer)
            g_d3madr = ctx->r[4].uw[0];
            return true;
        case 0x216b00u: // native site: D3_QWC (macroblocks x 32)
            g_d3qwc = ctx->r[2].uw[0] & 0xFFFFu;
            return true;
        case 0x216b54u: // native site: IPU_CMD (IDEC): the picture decoded, channel 3's interrupt raised
            decodePicture();
            // The transfer's end as the DMAC reports it: D_STAT's channel 3 bit (the kernel checks it when the game,
            // which masks the channel while it queues a picture, enables it again), and the interrupt.
            if (bozo::libs::Libraries *l = libraries(); l && l->dispatcher())
            {
                l->io.stat |= 1u << 3;
                l->dispatcher()->kernel().irq(true, 3);
            }
            return true;
        default:
            return false;
        }
    }

    void installIpu(Runtime &rt, bozo::port::Registry &r)
    {
        g_ram = rt.memory.ram();
        r.registerFunction(0x21b520, startStream);
        r.registerFunction(0x30eca8, stopDma);
        r.registerFunction(0x30ed90, restartDma);
        r.registerFunction(0x21b560, pictureFlags);
        r.registerFunction(0x30eee0, ipuSync); // sceIpuSync
    }

    void reportIpu()
    {
        std::printf("[amp:ipu] %zu streams, %llu pictures decoded, %llu failed\n", g_streams.size(),
                    (unsigned long long)g_decoded, (unsigned long long)g_failed);
    }
} // namespace amp
