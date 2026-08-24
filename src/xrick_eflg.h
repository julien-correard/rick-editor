// map_eflg_c: per-TILE (not per-block) environment/hazard flags -- the
// thing that actually decides whether a tile is solid, climbable, or
// lethal (which is what makes an enemy "turn into a corpse" on contact,
// see EDITEUR.md's investigation section). Confirmed against the real
// source (maps.h, maps.c):
//
//   #define MAP_EFLG_VERT   (0x80)  // vertical move only (usually on top of _CLIMB)
//   #define MAP_EFLG_SOLID  (0x40)  // solid block, can't go through
//   #define MAP_EFLG_SPAD   (0x20)  // super pad: solid, but sends entities skyward
//   #define MAP_EFLG_WAYUP  (0x10)  // solid except when going up
//   #define MAP_EFLG_FGND   (0x08)  // foreground (drawn in front of / hides entities)
//   #define MAP_EFLG_LETHAL (0x04)  // kills entities that touch it
//   #define MAP_EFLG_CLIMB  (0x02)  // entities can climb here
//   #define MAP_EFLG_01     (0x01)  // bit exists in the source, meaning undocumented
//
// This is a per-BANK table (one for tile bank 1/page 0, one for bank
// 2/page 1 -- same two banks as the Block Palette and block graphics),
// NOT per-submap and NOT per-block: it's a fixed property of a tile
// GRAPHIC (0-255), compiled into the game, entirely separate from the
// level's own map_bnums/map_blocks/marks data. There's nothing to
// "place" here -- editing it changes what a given tile graphic DOES
// everywhere it's used, across the whole bank.
//
// STORAGE FORMAT (confirmed in maps.c, map_eflg_expand()): each bank's
// 256 per-tile flag bytes are packed as exactly 8 run-length pairs of
// (count U8, value U8), i.e. 16 bytes per bank, 32 bytes total
// (map_eflg_c[MAP_NBR_EFLGC], MAP_NBR_EFLGC=0x20). The 8 counts per
// bank must sum to exactly 256. This is a HARD cap -- the real stock
// binary already uses all 8 runs in both banks (confirmed by decoding
// it), so edits that introduce more than 8 distinct boundaries can't be
// repacked; repackEflg() below reports that clearly instead of
// producing a broken table.
#pragma once

#include <array>
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>

#include "xrick_patch.h" // find_symbol_file_offset, patch_symbol

static const int MAP_NBR_EFLGC = 0x20; // 32: 16 compressed bytes x 2 banks

static const int EFLG_VERT = 0x80;
static const int EFLG_SOLID = 0x40;
static const int EFLG_SPAD = 0x20;
static const int EFLG_WAYUP = 0x10;
static const int EFLG_FGND = 0x08;
static const int EFLG_LETHAL = 0x04;
static const int EFLG_CLIMB = 0x02;
static const int EFLG_01 = 0x01;

using EflgTable = std::array<uint8_t, 256>; // per-tile flags, one bank

struct EflgData
{
    std::array<EflgTable, 2> bank{}; // [0] = tile bank 1/page 0, [1] = bank 2/page 1
};

// One editable row in the UI: covers tiles [start, endTile] (inclusive)
// with a single flags byte. `start` isn't stored -- it's always
// (previous row's endTile + 1), or 0 for the first row, so the list of
// rows always partitions 0-255 with no gaps by construction.
struct EflgRun { int endTile; uint8_t flags; };

// Expands the 16-byte compressed run stream for one bank into its full
// 256-entry table. Mirrors map_eflg_expand() in maps.c exactly: 8
// (count, value) pairs, each count U8 copies of value appended in turn.
inline EflgTable expandEflgBank(const uint8_t *compressed16)
{
    EflgTable out{};
    int k = 0;
    for (int i = 0; i < 16; )
    {
        uint8_t count = compressed16[i]; i++;
        uint8_t value = compressed16[i]; i++;
        while (count-- && k < 256) out[k++] = value;
    }
    return out;
}

// Derives the editable run list from a bank's full 256-entry table --
// just the run-length encoding of whatever's currently there. Always
// valid input (any 256-byte table has *some* RLE form); repackEflg() is
// where the 8-run ceiling actually gets enforced, since editing can
// grow the run count past what fits.
inline std::vector<EflgRun> eflgRunsFromTable(const EflgTable &t)
{
    std::vector<EflgRun> runs;
    int i = 0;
    while (i < 256)
    {
        int j = i;
        while (j < 256 && t[j] == t[i]) j++;
        runs.push_back(EflgRun{j - 1, t[i]});
        i = j;
    }
    return runs;
}

// Rebuilds a bank's full 256-entry table from an edited run list
// (each run's tiles are [previous endTile+1, endTile]).
inline EflgTable eflgTableFromRuns(const std::vector<EflgRun> &runs)
{
    EflgTable out{};
    int start = 0;
    for (auto &r : runs)
    {
        for (int t = start; t <= r.endTile && t < 256; t++) out[t] = r.flags;
        start = r.endTile + 1;
    }
    return out;
}

// The 32 compressed bytes from a real stock xrick binary (bank 1 then
// bank 2, 16 bytes each), decoded at startup -- same "vendor the real
// data" approach as connections_default.h / the sprite entity table.
inline EflgData defaultEflg()
{
    static const uint8_t stock[MAP_NBR_EFLGC] = {
        0x4d, 0x00, 0x0e, 0x02, 0x04, 0x04, 0x57, 0x08, 0x08, 0x18, 0x03, 0x68, 0x3b, 0x48, 0x04, 0x80,
        0x37, 0x00, 0x04, 0x02, 0x04, 0x04, 0x90, 0x08, 0x09, 0x18, 0x01, 0x68, 0x21, 0x48, 0x06, 0x80,
    };
    EflgData out;
    out.bank[0] = expandEflgBank(stock + 0);
    out.bank[1] = expandEflgBank(stock + 16);
    return out;
}

// Optional "import from a chosen binary" action (mirrors the same
// action already offered for connections): reads map_eflg_c directly
// via its ELF symbol rather than trusting the compiled-in stock table.
inline bool loadEflgFromXrickBinary(const fs::path &path, EflgData &out, std::string &err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "Could not open " + path.string(); return false; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (buf.empty()) { err = "File is empty or unreadable"; return false; }

    size_t off = 0, size = 0;
    if (!find_symbol_file_offset(buf, "map_eflg_c", off, size, err)) return false;
    if (size != (size_t)MAP_NBR_EFLGC)
    {
        err = "map_eflg_c has a different size (" + std::to_string(size)
            + " bytes, expected " + std::to_string(MAP_NBR_EFLGC) + ") -- likely an incompatible xrick build.";
        return false;
    }
    out.bank[0] = expandEflgBank(buf.data() + off);
    out.bank[1] = expandEflgBank(buf.data() + off + 16);
    err.clear();
    return true;
}

// Compresses both banks' 256-entry tables back into the fixed 32-byte
// run-length form. Fails with a clear, actionable message if either
// bank's table can't be expressed in 8 runs (splits any run longer than
// 255 tiles first, since the count byte is a U8 -- that alone can push
// a bank over the 8-run budget too).
inline bool repackEflg(const EflgData &data, uint8_t outCompressed32[/*32*/], std::string &err)
{
    for (int b = 0; b < 2; b++)
    {
        auto runs = eflgRunsFromTable(data.bank[b]);
        // Split any run longer than 255 tiles (count byte is a U8) --
        // rare (only possible for a single-flag bank), but handle it
        // rather than silently truncating.
        std::vector<EflgRun> split;
        int start = 0;
        for (auto &r : runs)
        {
            int len = r.endTile - start + 1;
            int t = start;
            while (len > 0)
            {
                int chunk = std::min(len, 255);
                split.push_back(EflgRun{t + chunk - 1, r.flags});
                t += chunk;
                len -= chunk;
            }
            start = r.endTile + 1;
        }
        if ((int)split.size() > 8)
        {
            err = "Tile bank " + std::to_string(b + 1) + " needs " + std::to_string(split.size())
                + " distinct flag ranges, but the original format only has room for 8 per bank. "
                  "Merge some adjacent ranges to the same flags (or undo recent edits) and try again.";
            return false;
        }
        uint8_t *out16 = outCompressed32 + b * 16;
        start = 0;
        int pairIdx = 0;
        for (auto &r : split)
        {
            int len = r.endTile - start + 1;
            out16[pairIdx * 2 + 0] = (uint8_t)len;
            out16[pairIdx * 2 + 1] = r.flags;
            pairIdx++;
            start = r.endTile + 1;
        }
        while (pairIdx < 8) { out16[pairIdx * 2 + 0] = 0; out16[pairIdx * 2 + 1] = 0; pairIdx++; }
    }
    err.clear();
    return true;
}

// Patches a copy of the binary's map_eflg_c symbol with the repacked
// bytes. Caller is expected to have already validated with repackEflg().
inline bool patchEflgSymbol(std::vector<uint8_t> &buf, const uint8_t compressed32[/*32*/], std::string &err)
{
    return patch_symbol(buf, "map_eflg_c", compressed32, MAP_NBR_EFLGC, err);
}
