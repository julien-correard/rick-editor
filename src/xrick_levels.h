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

#include "xrick_patch.h" // elf32_find_symbol_file_offset, elf32_patch_symbol
#include "connections_default.h"

static const int MAP_NBR_SUBMAPS = 0x2F; // 47
static const int MAP_NBR_CONNECT = 0x99; // 153
static const int SUBMAP_END_OF_LEVEL = 0xff;

struct SubmapInfo { int page = 0, bnum = 0, mark = 0; };

// dir: 0 = going down, 1 = going up.
// rowAbs: absolute row (this editor's coordinate space) of the trigger,
//   on the submap that owns this exit.
// targetSubmap: destination screen index, or SUBMAP_END_OF_LEVEL.
// targetRowAbs: absolute row Rick appears at on the target submap
//   (meaningless/unused when targetSubmap == SUBMAP_END_OF_LEVEL).
struct ConnectEntry { int dir = 0; int rowAbs = 0; int targetSubmap = SUBMAP_END_OF_LEVEL; int targetRowAbs = 0; };

struct ConnectionsData
{
    bool loaded = false;
    std::array<SubmapInfo, MAP_NBR_SUBMAPS> submaps;
    std::array<std::vector<ConnectEntry>, MAP_NBR_SUBMAPS> exits; // terminator implicit, not stored

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
                                                 const std::array<ConnectRaw, MAP_NBR_CONNECT> &rawConnect)
{
    ConnectionsData out;
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
        out.submaps[i] = {rawSubmaps[i].page, rawSubmaps[i].bnum, rawSubmaps[i].mark};

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
            out.exits[i].push_back(e);
            c++;
        }
    }
    out.loaded = true;
    return out;
}

inline ConnectionsData defaultConnections()
{
    return decodeConnectionsFromRaw(defaultSubmapsRaw, defaultConnectRaw);
}

inline bool loadXrickConnections(const fs::path &path, ConnectionsData &out, std::string &err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "Could not open " + path.string(); return false; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (buf.empty()) { err = "File is empty or unreadable"; return false; }

    size_t off = 0, size = 0;
    if (!elf32_find_symbol_file_offset(buf, "map_submaps", off, size, err)) return false;
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
    if (!elf32_find_symbol_file_offset(buf, "map_connect", coff, csize, err)) return false;
    if (csize != (size_t)MAP_NBR_CONNECT * 4)
    {
        err = "map_connect has an unexpected size (" + std::to_string(csize) + " bytes) -- incompatible xrick build.";
        return false;
    }
    std::array<ConnectRaw, MAP_NBR_CONNECT> rawConnect;
    for (int i = 0; i < MAP_NBR_CONNECT; i++)
        rawConnect[i] = {buf[coff + i * 4 + 0], buf[coff + i * 4 + 1], buf[coff + i * 4 + 2], buf[coff + i * 4 + 3]};

    out = decodeConnectionsFromRaw(rawSubmaps, rawConnect);
    err.clear();
    return true;
}

// Rebuilds the flat map_connect array (and each submap's `connect` start
// offset) from the per-submap exit lists currently in `data`, converting
// absolute rows back to each submap's local 0-255 range. Fails if a row
// doesn't fit in that range, or if the total number of slots needed
// exceeds the fixed capacity of the original binary's array.
inline bool repackConnections(ConnectionsData &data, std::string &err)
{
    std::vector<ConnectRaw> flat;
    flat.reserve(MAP_NBR_CONNECT);
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
    {
        int base = submapStartRow(data.submaps[i]);
        for (auto &e : data.exits[i])
        {
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

// --- .map v2: level layout + screen connections in one file ---------
//
// Format ("RKM2" magic):
//   header: magic[4]="RKM2", uint32 bnumsCount(=MAP_COUNT), uint32 submapCount(=47)
//   MAP_COUNT * int32          -- map_bnums, same encoding as v1
//   per submap (submapCount times):
//     int32 page, int32 bnum, int32 mark, int32 exitCount
//     exitCount * { int32 dir, int32 rowAbs, int32 targetSubmap, int32 targetRowAbs }
//
// Old v1 files ("RKMP" magic, bnums only) still open fine -- connections
// are simply left as they were (not reset) when loading one.
struct MapFileHeaderV2 { char magic[4]; uint32_t bnumsCount; uint32_t submapCount; };

inline bool saveMapFileWithConnections(const fs::path &path, const ConnectionsData &conn, std::string &err)
{
    FILE *f = std::fopen(path.string().c_str(), "wb");
    if (!f) { err = "Could not open file for writing"; return false; }
    MapFileHeaderV2 hdr{{'R', 'K', 'M', '2'}, (uint32_t)MAP_COUNT, (uint32_t)MAP_NBR_SUBMAPS};
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

    if (std::memcmp(magic, "RKM2", 4) != 0)
    {
        // Legacy v1 file (bnums only) -- load it as before, leave
        // whatever connections are currently loaded untouched.
        std::fclose(f);
        return loadMapFile(path, err);
    }

    MapFileHeaderV2 hdr{};
    bool ok = std::fread(&hdr, sizeof(hdr), 1, f) == 1
            && hdr.bnumsCount == (uint32_t)MAP_COUNT && hdr.submapCount == (uint32_t)MAP_NBR_SUBMAPS;
    if (!ok) { std::fclose(f); err = "Unsupported or mismatched .map file"; return false; }
    ok = std::fread(map_bnums, sizeof(int), MAP_COUNT, f) == (size_t)MAP_COUNT;
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
    if (!elf32_patch_symbol(buf, "map_bnums", bnumsBytes.data(), bnumsBytes.size(), err))
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
        if (!elf32_patch_symbol(buf, "map_submaps", conn->packedSubmaps.data(), conn->packedSubmaps.size(), err))
        {
            res.message = "Could not patch map_submaps: " + err;
            return res;
        }
        if (!elf32_patch_symbol(buf, "map_connect", conn->packedConnect.data(), conn->packedConnect.size(), err))
        {
            res.message = "Could not patch map_connect: " + err;
            return res;
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
