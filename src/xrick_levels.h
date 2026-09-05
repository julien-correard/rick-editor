// xrick_levels.h -- screen-to-screen connection graph (entrances/exits).
//
// Rick Dangerous has no horizontal scrolling: the level is a chain of
// fixed "submaps" (screens), and moving from one to the next is a
// vertical transition (walking off the top or bottom of the current
// screen). The real engine encodes this as:
//
//   map_submaps[47]  -- one entry per screen: tile page (0/1, selects
//                        tile bank 1/2), first block offset into
//                        map_bnums, and the index of its first exit in
//                        the shared map_connect[] array.
//   map_connect[153] -- shared, packed array of exits. Each submap's
//                        exits are a contiguous run starting at
//                        map_submaps[s].connect, terminated by an entry
//                        with dir==0xff. Each exit: dir (0=going down,
//                        1=going up), rowout (trigger row, LOCAL to the
//                        owning submap), submap (target screen, or
//                        0xff = end of this world), rowin (row to place
//                        Rick at on the target screen, LOCAL to the
//                        target submap).
//
// LOCAL vs ABSOLUTE rows: the real engine stores rowout/rowin as a
// single byte relative to each submap's own start row -- but crucially,
// in TILE-row units, not block-row units (confirmed directly from the
// original xrick source, maps.c: "we need to /4 map_frow to convert
// from tile rows to block rows"; map_frow is exactly what rowout/rowin
// get added to/subtracted from when jumping between submaps). A submap
// starts at tile-row (bnum/8)*4 = bnum/2 in this shared space (bnum/8
// is its start expressed as a BLOCK row, matching the main map view;
// multiply by 4 since one block-row is 4 tile-rows tall). Everywhere in
// this editor (UI, in-memory ConnectionsData, .map files) we use these
// ABSOLUTE tile-rows -- four times finer than the main map's block-row
// grid, so e.g. tile-row 60 lines up with block-row 15 there. The
// conversion back to local bytes (with range validation) only happens
// at the very last step, when patching a real xrick binary.
#pragma once

#include <array>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <algorithm>

#include "xrick_patch.h" // find_symbol_file_offset, patch_symbol
#include "connections_default.h"

static const int MAP_NBR_SUBMAPS = 0x2F; // 47
static const int MAP_NBR_CONNECT = 0x99; // 153
static const int SUBMAP_END_OF_LEVEL = 0xff;

struct SubmapInfo { int page = 0, bnum = 0, mark = 0; };

// Per-map player start position (matches xrick's map_t: {x, y, row,
// submap, tune}). x/y are pixel coordinates, row is the initial
// visible tile-row in absolute coordinates (submapStartRow + map_frow),
// submap is the starting submap index. The tune field (music file
// pointer) is stored in the ELF but not editable here since it
// requires pointer relocation. When patching, row is converted back
// to the raw submap-local map_frow = row - submapStartRow(submap).
// The raw value must be a multiple of 4 for map_expand() alignment;
// non-aligned values are rounded down at patch time.
struct MapStartInfo { int x = 0, y = 0, row = 0, submap = 0; };

// dir: 0 = going down, 1 = going up.
// rowAbs: absolute row (this editor's coordinate space) of the trigger,
//   on the submap that owns this exit. Meaningless for RUxF (left/right
//   transitions have no row trigger) but kept for UI bookkeeping.
// targetSubmap: destination screen index, or SUBMAP_END_OF_LEVEL.
// targetRowAbs: absolute row Rick appears at on the target submap
//   (meaningless/unused when targetSubmap == SUBMAP_END_OF_LEVEL).
// col: arrival tile column on the target submap (0-31). RUxF only;
//   legacy format has no column; set to 0 for legacy exits.
struct ConnectEntry { int dir = 0; int rowAbs = 0; int targetSubmap = SUBMAP_END_OF_LEVEL; int targetRowAbs = 0; int col = 0; };

struct ConnectionsData
{
    bool loaded = false;
    std::array<SubmapInfo, MAP_NBR_SUBMAPS> submaps;
    std::array<std::vector<ConnectEntry>, MAP_NBR_SUBMAPS> exits; // terminator implicit, not stored
    std::array<MapStartInfo, MAP_NBR_MAPS> mapStarts;

    // Filled by repackConnections(), consumed when patching a binary.
    std::vector<uint8_t> packedSubmaps;
    std::vector<uint8_t> packedConnect;
};

// Absolute TILE-row where this submap's own data starts (see the unit
// note above: bnum/8 is the BLOCK row, *4 to get the TILE row).
static inline int submapStartRow(const SubmapInfo &s) { return s.bnum / 2; }

// Decodes raw/local submap + connect arrays (as read from an ELF binary,
// or from the compiled-in defaults) into the editor's absolute-row model.
inline ConnectionsData decodeConnectionsFromRaw(const std::array<SubmapRaw, MAP_NBR_SUBMAPS> &rawSubmaps,
                                                 const std::array<ConnectRaw, MAP_NBR_CONNECT> &rawConnect,
                                                 const std::array<MapStartRaw, MAP_NBR_MAPS> &rawMapStarts)
{
    ConnectionsData out;
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
        out.submaps[i] = {rawSubmaps[i].page, rawSubmaps[i].bnum, rawSubmaps[i].mark};
    for (int i = 0; i < MAP_NBR_MAPS; i++)
    {
        int sm = rawMapStarts[i].submap;
        int base = (sm >= 0 && sm < MAP_NBR_SUBMAPS) ? submapStartRow(out.submaps[sm]) : 0;
        out.mapStarts[i] = {(int)rawMapStarts[i].x, (int)rawMapStarts[i].y, (int)rawMapStarts[i].row + base, sm};
    }

    for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
    {
        out.exits[i].clear();
        int c = rawSubmaps[i].connect;
        int guard = 0;
        int base = submapStartRow(out.submaps[i]);
        while (c >= 0 && c < MAP_NBR_CONNECT && rawConnect[c].dir != 0xff && guard++ < MAP_NBR_CONNECT)
        {
            const ConnectRaw &r = rawConnect[c];
            ConnectEntry e;
            e.dir = r.dir;
            e.rowAbs = base + r.rowout;
            e.targetSubmap = r.submap;
            e.targetRowAbs = (r.submap != SUBMAP_END_OF_LEVEL && r.submap < MAP_NBR_SUBMAPS)
                            ? submapStartRow(out.submaps[r.submap]) + r.rowin
                            : 0;
            e.col = 0; // legacy has no column
            out.exits[i].push_back(e);
            c++;
        }
    }
    out.loaded = true;
    return out;
}

inline ConnectionsData defaultConnections()
{
    return decodeConnectionsFromRaw(defaultSubmapsRaw, defaultConnectRaw, defaultMapStartsRaw);
}

inline bool loadXrickConnections(const fs::path &path, ConnectionsData &out, std::string &err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "Could not open " + path.string(); return false; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (buf.empty()) { err = "File is empty or unreadable"; return false; }

    size_t off = 0, size = 0;
    if (!find_symbol_file_offset(buf, "map_submaps", off, size, err)) return false;
    if (size != (size_t)MAP_NBR_SUBMAPS * 8)
    {
        err = "map_submaps has an unexpected size (" + std::to_string(size) + " bytes) -- incompatible xrick build.";
        return false;
    }
    std::array<SubmapRaw, MAP_NBR_SUBMAPS> rawSubmaps;
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
    {
        uint16_t page, bnum, connect, mark;
        std::memcpy(&page, &buf[off + i * 8 + 0], 2);
        std::memcpy(&bnum, &buf[off + i * 8 + 2], 2);
        std::memcpy(&connect, &buf[off + i * 8 + 4], 2);
        std::memcpy(&mark, &buf[off + i * 8 + 6], 2);
        rawSubmaps[i] = {page, bnum, connect, mark};
    }

    size_t coff = 0, csize = 0;
    if (!find_symbol_file_offset(buf, "map_connect", coff, csize, err)) return false;
    bool ruxfBin = (csize == MAP_NBR_CONNECT * 8); // RUxF: 8-byte connect_t
    if (csize != (size_t)MAP_NBR_CONNECT * 4 && !ruxfBin)
    {
        err = "map_connect has an unexpected size (" + std::to_string(csize) + " bytes) -- incompatible xrick build.";
        return false;
    }

    // RUxF connectors: connect_t {dir, submap, colin, pad, rowout(U16), rowin(U16)} = 8 bytes.
    // rowin AND rowout are absolute. Legacy: 4 bytes {dir,rowout,submap,rowin} local rows.
    if (ruxfBin)
    {
        for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
        {
            out.submaps[i] = {rawSubmaps[i].page, rawSubmaps[i].bnum, rawSubmaps[i].mark};
            out.exits[i].clear();
            int c = rawSubmaps[i].connect;
            while (c >= 0 && c < MAP_NBR_CONNECT)
            {
                uint8_t dir    = buf[coff + c * 8 + 0];
                uint8_t submap = buf[coff + c * 8 + 1];
                uint8_t colin  = buf[coff + c * 8 + 2];
                uint16_t rowout, rowin;
                std::memcpy(&rowout, &buf[coff + c * 8 + 4], 2);
                std::memcpy(&rowin,  &buf[coff + c * 8 + 6], 2);
                if (dir == 0xff) break;
                ConnectEntry e;
                e.dir = dir;
                e.targetSubmap = submap;
                e.targetRowAbs = rowin;   // already absolute
                e.rowAbs = rowout;        // exit tile row, absolute
                e.col = colin;
                out.exits[i].push_back(e);
                c++;
            }
        }
        out.loaded = true;
        err.clear();
        return true; // skip legacy decodeConnectionsFromRaw
    }

    std::array<ConnectRaw, MAP_NBR_CONNECT> rawConnect;
    for (int i = 0; i < MAP_NBR_CONNECT; i++)
        rawConnect[i] = {buf[coff + i * 4 + 0], buf[coff + i * 4 + 1], buf[coff + i * 4 + 2], buf[coff + i * 4 + 3]};

    out = decodeConnectionsFromRaw(rawSubmaps, rawConnect, defaultMapStartsRaw);

    // Try to load map_maps (per-map start positions) from the ELF.
    // Each entry is 12 bytes: {U16 x, U16 y, U16 row, U16 submap, char *tune}.
    // We read the 8-byte numeric portion and skip the 4-byte tune pointer.
    size_t moff = 0, msize = 0;
    if (find_symbol_file_offset(buf, "map_maps", moff, msize, err))
    {
        if (msize >= (size_t)MAP_NBR_MAPS * 12)
        {
            for (int i = 0; i < MAP_NBR_MAPS; i++)
            {
                uint16_t x, y, row, submap;
                std::memcpy(&x, &buf[moff + i * 12 + 0], 2);
                std::memcpy(&y, &buf[moff + i * 12 + 2], 2);
                std::memcpy(&row, &buf[moff + i * 12 + 4], 2);
                std::memcpy(&submap, &buf[moff + i * 12 + 6], 2);
                int sm = submap;
                int base = (sm >= 0 && sm < MAP_NBR_SUBMAPS) ? submapStartRow(out.submaps[sm]) : 0;
                out.mapStarts[i] = {x, y, (int)row + base, sm};
            }
        }
    }
    err.clear();
    return true;
}

// Rebuilds the flat map_connect array (and each submap's `connect` start
// offset) from the per-submap exit lists currently in `data`, converting
// The engine's map_chain() picks the exit of the matching direction whose
// rowout is the FIRST one >= Rick's current row when he walks off the edge
// (bracketing, with a fallback to the last one), which assumes each submap's
// connectors are packed in ASCENDING rowout order. The editor does not force
// that order on screen, so sort before packing -- otherwise (e.g. an
// upper-than-lower row stored first) a higher exit would bracket onto the
// lower connector. `data.exits[i]` itself is left untouched: the on-screen
// order, the .map round-trip and the UI stay exactly as the user arranged.
inline std::vector<size_t> sortedExitOrder(const std::vector<ConnectEntry> &v)
{
    std::vector<size_t> idx(v.size());
    for (size_t k = 0; k < v.size(); k++) idx[k] = k;
    std::stable_sort(idx.begin(), idx.end(),
        [&](size_t a, size_t b) { return v[a].rowAbs < v[b].rowAbs; });
    return idx;
}

// absolute rows back to each submap's local 0-255 range. Fails if a row
// doesn't fit in that range, or if the total number of slots needed
// exceeds the fixed capacity of the original binary's array.
inline bool repackConnections(ConnectionsData &data, std::string &err, bool ruxf = false)
{
    if (ruxf)
    {
        // RUxF connectors: connect_t is {dir(U8), submap(U8), colin(U8),
        // pad(U8), rowout(U16 LE), rowin(U16 LE)} = 8 bytes per slot (see the
        // engine's include/maps.h). rowin AND rowout are ABSOLUTE (no per-
        // submap base). colin is the arrival tile column; rowout is the exit
        // tile row on this submap, used by the engine to pick the right
        // connector by Rick's height (bracketing). Capacity is
        // MAP_NBR_CONNECT (153) slots = 1224 bytes.
        const int RUxF_CONNECT_STRIDE = 8;
        std::vector<uint8_t> flat;
        flat.reserve(MAP_NBR_CONNECT * RUxF_CONNECT_STRIDE);
        for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
        {
            for (size_t k : sortedExitOrder(data.exits[i]))
            {
                auto &e = data.exits[i][k];
                if (e.targetSubmap != SUBMAP_END_OF_LEVEL && (e.targetSubmap < 0 || e.targetSubmap >= MAP_NBR_SUBMAPS))
                {
                    err = "Submap " + std::to_string(i) + ": invalid target submap " + std::to_string(e.targetSubmap);
                    return false;
                }
                if (e.targetRowAbs < 0 || e.targetRowAbs > 65535)
                {
                    err = "Submap " + std::to_string(i) + ": target row " + std::to_string(e.targetRowAbs)
                        + " out of RUxF range (0-65535).";
                    return false;
                }
                if (e.rowAbs < 0 || e.rowAbs > 65535)
                {
                    err = "Submap " + std::to_string(i) + ": exit row " + std::to_string(e.rowAbs)
                        + " out of RUxF range (0-65535).";
                    return false;
                }
                uint8_t col = (uint8_t)std::clamp(e.col, 0, 31);
                uint8_t dir = (uint8_t)(e.dir & 1);
                uint8_t target = (uint8_t)e.targetSubmap;
                uint16_t rowout = (uint16_t)e.rowAbs;
                uint16_t rowin  = (uint16_t)e.targetRowAbs;
                uint8_t outLo = (uint8_t)(rowout & 0xff), outHi = (uint8_t)(rowout >> 8);
                uint8_t inLo  = (uint8_t)(rowin & 0xff),  inHi  = (uint8_t)(rowin >> 8);
                flat.insert(flat.end(), {dir, target, col, 0x00, outLo, outHi, inLo, inHi});
            }
            flat.insert(flat.end(), {0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}); // end marker
        }
        if ((int)(flat.size() / RUxF_CONNECT_STRIDE) > MAP_NBR_CONNECT)
        {
            err = "Too many exits (" + std::to_string(flat.size() / RUxF_CONNECT_STRIDE)
                + " slots needed) -- the RUxF engine only has room for "
                + std::to_string(MAP_NBR_CONNECT) + " total (across all submaps, "
                "including one end-marker per submap).";
            return false;
        }
        while ((int)(flat.size() / RUxF_CONNECT_STRIDE) < MAP_NBR_CONNECT)
            flat.insert(flat.end(), {0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        data.packedConnect = flat;

        data.packedSubmaps.assign(MAP_NBR_SUBMAPS * 8, 0);
        std::array<uint16_t, MAP_NBR_SUBMAPS> connectStart{};
        {
            int idx = 0;
            for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
            {
                connectStart[i] = (uint16_t)idx;
                idx += (int)data.exits[i].size() + 1;
            }
        }
        for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
        {
            uint16_t page = (uint16_t)data.submaps[i].page;
            uint16_t bnum = (uint16_t)data.submaps[i].bnum;
            uint16_t connect = connectStart[i];
            uint16_t mark = (uint16_t)data.submaps[i].mark;
            std::memcpy(&data.packedSubmaps[i * 8 + 0], &page, 2);
            std::memcpy(&data.packedSubmaps[i * 8 + 2], &bnum, 2);
            std::memcpy(&data.packedSubmaps[i * 8 + 4], &connect, 2);
            std::memcpy(&data.packedSubmaps[i * 8 + 6], &mark, 2);
        }
        err.clear();
        return true;
    }
    std::vector<ConnectRaw> flat;
    flat.reserve(MAP_NBR_CONNECT);
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
    {
        int base = submapStartRow(data.submaps[i]);
        for (size_t k : sortedExitOrder(data.exits[i]))
        {
            auto &e = data.exits[i][k];
            int localOut = e.rowAbs - base;
            if (localOut < 0 || localOut > 255)
            {
                err = "Submap " + std::to_string(i) + ": row " + std::to_string(e.rowAbs)
                    + " is too far from this submap's own rows (" + std::to_string(base)
                    + "-" + std::to_string(base + 255) + ") to fit the original format.";
                return false;
            }
            int localIn = 0;
            if (e.targetSubmap != SUBMAP_END_OF_LEVEL)
            {
                if (e.targetSubmap < 0 || e.targetSubmap >= MAP_NBR_SUBMAPS)
                {
                    err = "Submap " + std::to_string(i) + ": invalid target submap " + std::to_string(e.targetSubmap);
                    return false;
                }
                int targetBase = submapStartRow(data.submaps[e.targetSubmap]);
                localIn = e.targetRowAbs - targetBase;
                if (localIn < 0 || localIn > 255)
                {
                    err = "Submap " + std::to_string(i) + " -> " + std::to_string(e.targetSubmap)
                        + ": target row " + std::to_string(e.targetRowAbs) + " is too far from submap "
                        + std::to_string(e.targetSubmap) + "'s own rows to fit the original format.";
                    return false;
                }
            }
            flat.push_back(ConnectRaw{(uint8_t)e.dir, (uint8_t)localOut, (uint8_t)e.targetSubmap, (uint8_t)localIn});
        }
        flat.push_back(ConnectRaw{0xff, 0, 0xff, 0}); // end-of-chain marker
    }
    if ((int)flat.size() > MAP_NBR_CONNECT)
    {
        err = "Too many exits (" + std::to_string(flat.size()) + " slots needed) -- "
              "the original binary only has room for " + std::to_string(MAP_NBR_CONNECT)
              + " total (across all submaps, including one end-marker per submap). "
              "Remove some exits, or delete unused submaps.";
        return false;
    }
    while ((int)flat.size() < MAP_NBR_CONNECT) flat.push_back(ConnectRaw{0xff, 0, 0xff, 0});

    // Recompute each submap's `connect` start offset for the packed layout.
    std::array<uint16_t, MAP_NBR_SUBMAPS> connectStart{};
    {
        int idx = 0;
        for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
        {
            connectStart[i] = (uint16_t)idx;
            idx += (int)data.exits[i].size() + 1; // +1 for its end-marker
        }
    }

    data.packedConnect.assign(MAP_NBR_CONNECT * 4, 0);
    for (int i = 0; i < MAP_NBR_CONNECT; i++)
    {
        data.packedConnect[i * 4 + 0] = flat[i].dir;
        data.packedConnect[i * 4 + 1] = flat[i].rowout;
        data.packedConnect[i * 4 + 2] = flat[i].submap;
        data.packedConnect[i * 4 + 3] = flat[i].rowin;
    }

    data.packedSubmaps.assign(MAP_NBR_SUBMAPS * 8, 0);
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
    {
        uint16_t page = (uint16_t)data.submaps[i].page;
        uint16_t bnum = (uint16_t)data.submaps[i].bnum;
        uint16_t connect = connectStart[i];
        uint16_t mark = (uint16_t)data.submaps[i].mark;
        std::memcpy(&data.packedSubmaps[i * 8 + 0], &page, 2);
        std::memcpy(&data.packedSubmaps[i * 8 + 2], &bnum, 2);
        std::memcpy(&data.packedSubmaps[i * 8 + 4], &connect, 2);
        std::memcpy(&data.packedSubmaps[i * 8 + 6], &mark, 2);
    }
    err.clear();
    return true;
}

// Disconnects submap `s`: clears its own exits, and redirects any OTHER
// submap's exit that targeted it to SUBMAP_END_OF_LEVEL. Returns the
// number of incoming links that were redirected. The submap's tiles
// (page/bnum) are left untouched -- the underlying array can't be
// shrunk, so "deleting" means making it unreachable, not erasing it.
inline int disconnectSubmap(ConnectionsData &data, int s)
{
    data.exits[s].clear();
    int redirected = 0;
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
    {
        if (i == s) continue;
        for (auto &e : data.exits[i])
        {
            if (e.targetSubmap == s)
            {
                e.targetSubmap = SUBMAP_END_OF_LEVEL;
                e.targetRowAbs = 0;
                redirected++;
            }
        }
    }
    return redirected;
}

// One-off diagnostic + fix for a specific known quirk in the stock
// data: starting at the first submap (by bnum) whose bnum isn't a
// multiple of 8, a run of submaps have their block data packed
// starting a few COLUMNS into a block-row instead of at its start
// (bnum used as a flat, row-agnostic index into map_bnums -- confirmed
// in maps.c's map_expand(): `map_bnums[pbnum]` with pbnum computed by
// adding bnum directly, no /8 conversion, so it reads 8 consecutive
// flat entries per screen-row regardless of which "row" they land in
// under this editor's own fixed 8-column grid). Confirmed harmless for
// actual gameplay (the real engine's flat/sliding-window read handles
// it correctly either way) -- but it means this editor's own
// block-row-aligned canvas rendering, and its "Submap N" boundary
// labels, don't accurately represent where these submaps' content
// really starts, which looks like "everything's shifted".
//
// This only touches the block DATA and each affected submap's own
// `bnum` -- never any mark's or connection's absolute row (those are
// untouched, so nothing visually moves in-game; only the block-grid
// alignment changes) -- and only proceeds if the exact known-safe
// precondition holds (the few blocks right before the misaligned
// submap's start are genuinely unused, i.e. all zero): otherwise it
// refuses and explains why, rather than guessing.
struct BlockShiftFixResult { bool applied = false; std::string message; };

inline BlockShiftFixResult fixMisalignedBlockRun(ConnectionsData &conn)
{
    BlockShiftFixResult res;
    std::vector<int> order;
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return conn.submaps[a].bnum < conn.submaps[b].bnum; });

    int firstBad = -1;
    for (int idx : order)
        if (conn.submaps[idx].bnum % 8 != 0) { firstBad = idx; break; }
    if (firstBad < 0) { res.message = "No misaligned submaps found -- nothing to fix."; return res; }

    int bnum = conn.submaps[firstBad].bnum;
    int residual = bnum % 8;
    int gapStart = bnum - residual;
    for (int i = gapStart; i < bnum; i++)
    {
        if (map_bnums[i] != 0)
        {
            res.message = "Submap " + std::to_string(firstBad) + " starts at a non-aligned block ("
                + std::to_string(bnum) + "), but the " + std::to_string(residual)
                + " block(s) right before it aren't empty (block " + std::to_string(i) + " = "
                + std::to_string(map_bnums[i]) + ") -- refusing to shift, this doesn't match the "
                  "one known-safe pattern this fix handles.";
            return res;
        }
    }

    std::vector<int> affected;
    for (int idx : order) if (conn.submaps[idx].bnum >= bnum) affected.push_back(idx);
    for (int idx : affected)
    {
        if (conn.submaps[idx].bnum % 8 != residual)
        {
            res.message = "Submap " + std::to_string(idx) + " (block " + std::to_string(conn.submaps[idx].bnum)
                + ") doesn't share submap " + std::to_string(firstBad) + "'s offset -- refusing to shift, "
                  "the run of affected submaps isn't as expected.";
            return res;
        }
    }

    int total = MAP_COUNT;
    for (int i = gapStart; i < total - residual; i++)
        map_bnums[i] = map_bnums[i + residual];
    for (int i = total - residual; i < total; i++)
        map_bnums[i] = 0;

    for (int idx : affected)
        conn.submaps[idx].bnum -= residual;

    res.applied = true;
    res.message = "Realigned " + std::to_string(affected.size()) + " submap(s) starting at submap "
        + std::to_string(firstBad) + " (block " + std::to_string(bnum) + " -> " + std::to_string(gapStart)
        + ") by absorbing " + std::to_string(residual) + " previously-unused padding block(s). "
          "Nothing displayed in-game changes (marks/connections keep their own absolute rows) -- "
          "only the block-grid alignment. Save/re-patch to keep this fix.";
    return res;
}

// --- .map v3: level layout + screen connections + map starts in one file --
//
// Format ("RKM3" magic):
//   header: magic[4]="RKM3", uint32 bnumsCount(=MAP_COUNT), uint32 submapCount(=47), uint32 mapCount(=5)
//   MAP_COUNT * int32          -- map_bnums, same encoding as v1
//   per submap (submapCount times):
//     int32 page, int32 bnum, int32 mark, int32 exitCount
//     exitCount * { int32 dir, int32 rowAbs, int32 targetSubmap, int32 targetRowAbs }
//   per map (mapCount times):
//     int32 x, int32 y, int32 row, int32 submap
//
// Old v2 files ("RKM2" magic, bnums + connections only) still open
// fine -- map starts are left at their defaults. Old v1 files ("RKMP"
// magic, bnums only) also still work.
struct MapFileHeaderV3Conn { char magic[4]; uint32_t bnumsCount; uint32_t submapCount; uint32_t mapCount; };

inline bool saveMapFileWithConnections(const fs::path &path, const ConnectionsData &conn, std::string &err)
{
    FILE *f = std::fopen(path.string().c_str(), "wb");
    if (!f) { err = "Could not open file for writing"; return false; }
    MapFileHeaderV3Conn hdr{{'R', 'K', 'M', '3'}, (uint32_t)MAP_COUNT, (uint32_t)MAP_NBR_SUBMAPS, (uint32_t)MAP_NBR_MAPS};
    bool ok = std::fwrite(&hdr, sizeof(hdr), 1, f) == 1
           && std::fwrite(map_bnums, sizeof(int), MAP_COUNT, f) == (size_t)MAP_COUNT;
    for (int i = 0; ok && i < MAP_NBR_SUBMAPS; i++)
    {
        int32_t page = conn.submaps[i].page, bnum = conn.submaps[i].bnum, mark = conn.submaps[i].mark;
        int32_t exitCount = (int32_t)conn.exits[i].size();
        ok = ok && std::fwrite(&page, 4, 1, f) == 1 && std::fwrite(&bnum, 4, 1, f) == 1
                && std::fwrite(&mark, 4, 1, f) == 1 && std::fwrite(&exitCount, 4, 1, f) == 1;
        for (auto &e : conn.exits[i])
        {
            int32_t vals[4] = {e.dir, e.rowAbs, e.targetSubmap, e.targetRowAbs};
            ok = ok && std::fwrite(vals, 4, 4, f) == 4;
        }
    }
    for (int i = 0; ok && i < MAP_NBR_MAPS; i++)
    {
        int sm = conn.mapStarts[i].submap;
        int base = (sm >= 0 && sm < MAP_NBR_SUBMAPS) ? submapStartRow(conn.submaps[sm]) : 0;
        int rawRow = conn.mapStarts[i].row - base;
        int32_t vals[4] = {conn.mapStarts[i].x, conn.mapStarts[i].y, rawRow, conn.mapStarts[i].submap};
        ok = ok && std::fwrite(vals, 4, 4, f) == 4;
    }
    std::fclose(f);
    if (!ok) { err = "Write error"; return false; }
    return true;
}

inline bool loadMapFileWithConnections(const fs::path &path, ConnectionsData &conn, std::string &err)
{
    FILE *f = std::fopen(path.string().c_str(), "rb");
    if (!f) { err = "Could not open file"; return false; }
    char magic[4];
    if (std::fread(magic, 1, 4, f) != 4) { std::fclose(f); err = "Not a valid .map file"; return false; }
    std::fseek(f, 0, SEEK_SET);

    if (std::memcmp(magic, "RKMP", 4) != 0 && std::memcmp(magic, "RKM2", 4) != 0 && std::memcmp(magic, "RKM3", 4) != 0)
    {
        std::fclose(f);
        err = "Unknown .map file format";
        return false;
    }

    if (std::memcmp(magic, "RKMP", 4) == 0)
    {
        // Legacy v1 file (bnums only) -- load it as before, leave
        // whatever connections are currently loaded untouched.
        std::fclose(f);
        return loadMapFile(path, err);
    }

    bool isV3 = (std::memcmp(magic, "RKM3", 4) == 0);

    MapFileHeaderV3Conn hdr{};
    if (isV3)
    {
        bool ok = std::fread(&hdr, sizeof(hdr), 1, f) == 1
                && hdr.bnumsCount == (uint32_t)MAP_COUNT && hdr.submapCount == (uint32_t)MAP_NBR_SUBMAPS;
        if (!ok) { std::fclose(f); err = "Unsupported or mismatched .map file"; return false; }
    }
    else
    {
        // v2 header: read just the first 12 bytes (magic + 2 uint32s), zero the third
        uint32_t bc, sc;
        bool ok = std::fread(hdr.magic, 4, 1, f) == 1
                && std::fread(&bc, 4, 1, f) == 1
                && std::fread(&sc, 4, 1, f) == 1;
        hdr.bnumsCount = bc; hdr.submapCount = sc; hdr.mapCount = 0;
        if (!ok || hdr.bnumsCount != (uint32_t)MAP_COUNT || hdr.submapCount != (uint32_t)MAP_NBR_SUBMAPS)
        { std::fclose(f); err = "Unsupported or mismatched .map file"; return false; }
    }

    bool ok = std::fread(map_bnums, sizeof(int), MAP_COUNT, f) == (size_t)MAP_COUNT;
    if (!ok) { std::fclose(f); err = "Read error (level layout)"; return false; }

    ConnectionsData loaded;
    loaded.loaded = true;
    for (int i = 0; ok && i < MAP_NBR_SUBMAPS; i++)
    {
        int32_t page, bnum, mark, exitCount;
        ok = std::fread(&page, 4, 1, f) == 1 && std::fread(&bnum, 4, 1, f) == 1
          && std::fread(&mark, 4, 1, f) == 1 && std::fread(&exitCount, 4, 1, f) == 1;
        if (!ok) break;
        loaded.submaps[i] = {(int)page, (int)bnum, (int)mark};
        loaded.exits[i].clear();
        for (int e = 0; e < exitCount && ok; e++)
        {
            int32_t vals[4];
            ok = std::fread(vals, 4, 4, f) == 4;
            if (ok) loaded.exits[i].push_back(ConnectEntry{(int)vals[0], (int)vals[1], (int)vals[2], (int)vals[3]});
        }
    }

    // v3: also read per-map start positions
    if (ok && isV3 && hdr.mapCount == (uint32_t)MAP_NBR_MAPS)
    {
        for (int i = 0; i < MAP_NBR_MAPS; i++)
        {
            int32_t vals[4];
            if (std::fread(vals, 4, 4, f) != 4) { ok = false; break; }
            int sm = (int)vals[3];
            int base = (sm >= 0 && sm < MAP_NBR_SUBMAPS) ? submapStartRow(loaded.submaps[sm]) : 0;
            loaded.mapStarts[i] = {(int)vals[0], (int)vals[1], (int)vals[2] + base, sm};
        }
    }

    std::fclose(f);
    if (!ok) { err = "Read error (screen connections)"; return false; }
    conn = loaded;
    return true;
}


inline PatchResult patchXrickBinaryFull(const fs::path &xrickPath, ConnectionsData *conn)
{
    PatchResult res;

    std::ifstream in(xrickPath, std::ios::binary);
    if (!in) { res.message = "Could not open " + xrickPath.string(); return res; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (buf.empty()) { res.message = "File is empty or unreadable"; return res; }

    std::string err;
    std::vector<uint8_t> bnumsBytes = map_bnums_as_bytes();
    if (!patch_symbol(buf, "map_bnums", bnumsBytes.data(), bnumsBytes.size(), err))
    {
        res.message = "Could not patch the level layout: " + err;
        return res;
    }

    bool patchedConnections = false;
    if (conn && conn->loaded)
    {
        if (!repackConnections(*conn, err))
        {
            res.message = "Could not patch the screen connections: " + err;
            return res;
        }
        if (!patch_symbol(buf, "map_submaps", conn->packedSubmaps.data(), conn->packedSubmaps.size(), err))
        {
            res.message = "Could not patch map_submaps: " + err;
            return res;
        }
        if (!patch_symbol(buf, "map_connect", conn->packedConnect.data(), conn->packedConnect.size(), err))
        {
            res.message = "Could not patch map_connect: " + err;
            return res;
        }

        // Patch map_maps (per-map start positions). Each entry is 12
        // bytes: {U16 x, U16 y, U16 row, U16 submap, char *tune}.
        // We read the existing tune pointers and preserve them.
        {
            size_t moff = 0, msize = 0;
            std::string merr;
            if (find_symbol_file_offset(buf, "map_maps", moff, msize, merr)
                && msize >= (size_t)MAP_NBR_MAPS * 12)
            {
                // Read existing tune pointers (4 bytes at offset 8 of each 12-byte entry)
                std::vector<uint8_t> patched(MAP_NBR_MAPS * 12);
                for (int i = 0; i < MAP_NBR_MAPS; i++)
                {
                    uint16_t x = (uint16_t)conn->mapStarts[i].x;
                    uint16_t y = (uint16_t)conn->mapStarts[i].y;
                    int sm = conn->mapStarts[i].submap;
                    int base = (sm >= 0 && sm < MAP_NBR_SUBMAPS) ? submapStartRow(conn->submaps[sm]) : 0;
                    int rawRow = conn->mapStarts[i].row - base;
                    rawRow = (rawRow / 4) * 4;
                    if (rawRow < 0) rawRow = 0;
                    if (rawRow > 255) rawRow = 255;
                    uint16_t row = (uint16_t)rawRow;
                    uint16_t submap = (uint16_t)conn->mapStarts[i].submap;
                    std::memcpy(&patched[i * 12 + 0], &x, 2);
                    std::memcpy(&patched[i * 12 + 2], &y, 2);
                    std::memcpy(&patched[i * 12 + 4], &row, 2);
                    std::memcpy(&patched[i * 12 + 6], &submap, 2);
                    // Preserve existing tune pointer (bytes 8-11)
                    std::memcpy(&patched[i * 12 + 8], &buf[moff + i * 12 + 8], 4);
                }
                std::memcpy(buf.data() + moff, patched.data(), MAP_NBR_MAPS * 12);
            }
        }

        patchedConnections = true;
    }

    fs::path outPath = xrickPath;
    outPath.replace_filename(xrickPath.stem().string() + "_patched" + xrickPath.extension().string());

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) { res.message = "Could not create " + outPath.string(); return res; }
    out.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)buf.size());
    out.close();

    std::error_code ec;
    fs::permissions(outPath, fs::status(xrickPath, ec).permissions(), fs::perm_options::replace, ec);

    res.ok = true;
    res.outputPath = outPath;
    res.message = "Patched level written to " + outPath.string()
                + (patchedConnections ? " (level layout + screen connections)" : " (level layout only)");
    return res;
}
