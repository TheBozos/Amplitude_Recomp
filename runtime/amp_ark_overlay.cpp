// SPDX-License-Identifier: MIT
// The archive overlay: files of GEN/MAIN.ARK read with the port's own bytes (runtime/amp_ark_overlay.h).
//
// The game reads the archive with sceCdRead, 32 sectors at a time from its first sector (sceCdSearchFile's answer),
// in the order the files lie in it, and finds each file by its table of contents' offset and size. Moving a file's
// entry elsewhere (past the disc's end) broke the boot (a jump to 0 at frame 82, before the file was even read): the
// reader goes by the offsets' order. So a replaced file keeps its place and its size: the port's bytes, padded with
// zeros to the original size, are laid over the sectors the engine read wherever they cover the file.
#include "amp_ark_overlay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace amp::ark
{
    namespace
    {
        constexpr uint64_t kSector = 2048u;

        struct Replacement
        {
            std::string name;
            uint32_t offset = 0, size = 0; // in the archive
            std::vector<uint8_t> bytes;    // `size` bytes
        };

        std::vector<Replacement> g_files;
        uint32_t g_arkLbn = 0;
    } // namespace

    bool replace(const std::string &name, uint32_t offset, uint32_t size, std::vector<uint8_t> bytes)
    {
        if (bytes.size() > size)
        {
            std::fprintf(stderr, "[amp:ark] %s: %zu bytes, more than its %u in the archive: not replaced\n", name.c_str(),
                         bytes.size(), size);
            return false;
        }
        bytes.resize(size, 0);
        for (Replacement &r : g_files)
            if (r.offset == offset)
            {
                r.bytes = std::move(bytes);
                return true;
            }
        g_files.push_back({name, offset, size, std::move(bytes)});
        return true;
    }

    void loadFolder(const std::string &folder)
    {
        for (const Known &k : known())
            if (FILE *f = std::fopen((folder + "/" + k.name).c_str(), "rb"))
            {
                std::vector<uint8_t> bytes;
                uint8_t buffer[65536];
                for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;)
                    bytes.insert(bytes.end(), buffer, buffer + n);
                std::fclose(f);
                std::printf("[amp:ark] %s from %s (%zu bytes)\n", k.name, folder.c_str(), bytes.size());
                replace(k.name, k.offset, k.size, std::move(bytes));
            }
    }

    const std::vector<Known> &known()
    {
        // The USA disc's entries (SCUS_972.58): the menus' definitions, the panels' scenes, the messages (the help line).
        static const std::vector<Known> files = {{"meta.ui.bin", 0x4d2b55u, 0x2c039u},
                                                 {"o_main.rnd", 0xc9f814eu, 0xd7cu},
                                                 {"s_pause.rnd", 0xcb30990u, 0xc64u},
                                                 {"s_r_pause.rnd", 0xcb360d6u, 0xa7bu},
                                                 {"m_g_pause.rnd", 0x510e15u, 0xc3du},
                                                 {"m_r_pause.rnd", 0x511a52u, 0xc0fu},
                                                 {"english_messages.txt.bin", 0x385284u, 0x1598du}};
        return files;
    }

    void setArchiveSector(uint32_t lbn) { g_arkLbn = lbn; }
    uint32_t archiveSector() { return g_arkLbn; }

    bool active() { return !g_files.empty() && g_arkLbn != 0; }

    void overlay(uint8_t *ram, uint32_t lbn, uint32_t sectors, uint32_t to)
    {
        if (!active() || lbn < g_arkLbn)
            return;
        const uint64_t from = (lbn - g_arkLbn) * kSector, end = from + sectors * kSector;
        for (const Replacement &r : g_files)
        {
            const uint64_t a = std::max<uint64_t>(from, r.offset), b = std::min<uint64_t>(end, uint64_t(r.offset) + r.size);
            if (a >= b)
                continue;
            std::memcpy(ram + ((to + (a - from)) & 0x1FFFFFFu), r.bytes.data() + (a - r.offset), static_cast<size_t>(b - a));
        }
    }
} // namespace amp::ark
