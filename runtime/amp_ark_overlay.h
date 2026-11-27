// SPDX-License-Identifier: MIT
// The archive overlay: files of GEN/MAIN.ARK read with bytes the port makes (the menus' PC SETTINGS entry).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amp::ark
{
    // The archive's files the port may replace: their offset and size in the archive (the USA disc).
    struct Known
    {
        const char *name;
        uint32_t offset, size;
    };
    const std::vector<Known> &known();

    // The file at `offset` (`size` bytes in the archive) read as `bytes`, padded with zeros to `size`; false (and not
    // replaced) when they are more than `size`: a file keeps its place and its size (amp_ark_overlay.cpp).
    bool replace(const std::string &name, uint32_t offset, uint32_t size, std::vector<uint8_t> bytes);
    // (development) AMP_ARK_OVERLAY_DIR: the known files read from that folder when there.
    void loadFolder(const std::string &folder);
    // The archive's first sector on the disc (sceCdSearchFile's answer for GEN/MAIN.ARK).
    void setArchiveSector(uint32_t lbn);
    uint32_t archiveSector();
    bool active();
    // After a sceCdRead of `sectors` from `lbn` into `to`: the replaced files' bytes laid over what it read.
    void overlay(uint8_t *ram, uint32_t lbn, uint32_t sectors, uint32_t to);
} // namespace amp::ark
