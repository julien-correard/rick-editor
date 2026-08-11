// xrick_marks.h -- entity/sprite placements ("marks") per submap.
//
// Structurally almost identical to the screen-connection graph in
// xrick_levels.h: map_marks[523] is a single shared, packed array; each
// submap owns a contiguous run starting at map_submaps[s].mark,
// terminated by an entry with row==0xff. Verified against this
// project's binary: every submap's run does end with such a marker, and
// the table is (like map_connect) already 100% full: 476 real marks +
// 47 end-markers = 523/523, zero spare slots.
//
//   mark_t { U8 row; U8 ent; U8 flags; U8 xy; U8 lt; }
//     row:   trigger row, LOCAL to the owning submap, in the same
//            TILE-row unit as connect_t.rowout (see xrick_levels.h) --
//            converted to/from an ABSOLUTE tile-row here. Confirmed
//            directly from the original source (xrick/src/ents.c,
//            ent_actvis()): `y = (xy&7) + (row&0xf8) - map_frow`, and
//            map_frow is documented (maps.c) as tile-row-scaled. In the
//            stock data checked against, every single row value (523/523)
//            is already an exact multiple of 8 -- entities always sit on
//            2-block-row-aligned positions in practice.
//     ent:   entity/sprite type id. No official names are baked into
//            this binary (no string table for them) -- exposed as a raw
//            number. Values 4-73 are used in the stock data checked
//            against; 0-3 never appear (possibly reserved for Rick
//            himself / special-cased entities).
//     xy:    packed position: `x = xy & 0xf8` (tile column in PIXELS,
//            i.e. always a multiple of 8, 0-248) and a 0-7 fine
//            vertical offset (`xy & 0x07`) added into the same row-scale
//            sum as `row` before the final *8 -> pixel conversion the
//            engine does at runtime. Confirmed against ents.c.
//     flags: MAP_MARK_NACT (0x80) = "not active anymore" is the only
//            documented bit; other bits are set in the stock data too
//            (likely per-entity behavior flags) but aren't decoded here
//            -- exposed as a raw byte.
//     lt:    packed trigger info (trig_x / lat & trig_y), used by some
//            trap-like entities. Not decoded -- exposed as a raw byte.
#pragma once

#include <array>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <fstream>

#include "xrick_levels.h" // submapStartRow, ConnectionsData, elf32_* helpers, connections_default.h

static const int MAP_NBR_MARKS = 0x20B; // 523

struct MarkEntry
{
    int rowAbs = 0;   // absolute row (this editor's coordinate space)
    int col = 0;      // tile column within the submap, 0-31
    int fineY = 0;     // 0-7, added to `row` (same scale, before the *8 -> pixel conversion) -- NOT a small pixel nudge, see decode below
    int ent = 4;       // entity/sprite type id
    int flags = 0;     // raw, not decoded (top bit = MAP_MARK_NACT)
    // Trigger point, decoded from `lt` (confirmed in ents.c, ent_reset()):
    // some entity types (e.g. arrow traps) react to Rick's position near
    // (trigCol, this-mark's-row + trigRowOffset) rather than their own
    // (col, row) -- that's why e.g. an arrow trap's firing spot can look
    // "off" from the trap's own drawn position: it's meant to be. Not
    // every entity type uses this; for those that don't it's inert.
    int trigCol = 0;      // 0-31, tile column of the trigger point
    int trigRowOffset = 0; // 0-7, added to this mark's own row for the trigger's row
};

struct MarksData
{
    bool loaded = false;
    std::array<std::vector<MarkEntry>, MAP_NBR_SUBMAPS> marks; // terminator implicit

    std::vector<uint8_t> packedMarks;   // filled by repackMarks()
    std::vector<uint16_t> markStart;    // per-submap start offset, filled by repackMarks()
};

// Absolute pixel-scale row of a mark's OWN position (row + fineY, both
// additive at the same scale before the engine's final *8 -> pixel step
// -- confirmed in ents.c: `y = (xy&7) + (row&0xf8) - map_frow`). This is
// NOT simply `rowAbs`: fineY contributes at the SAME weight as a whole
// row unit, not a fraction of one.
static inline int markEffectiveRow(const MarkEntry &m) { return m.rowAbs + m.fineY; }
// Same for the trigger point (uses this mark's own row, per source).
static inline int markTriggerRow(const MarkEntry &m) { return m.rowAbs + m.trigRowOffset; }

inline MarksData decodeMarksFromRaw(const std::array<SubmapRaw, MAP_NBR_SUBMAPS> &rawSubmaps,
                                     const std::array<MarkRaw, MAP_NBR_MARKS> &rawMarks)
{
    MarksData out;
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
    {
        out.marks[i].clear();
        int base = rawSubmaps[i].bnum / 2; // tile-row start (see xrick_levels.h's submapStartRow doc)
        int c = rawSubmaps[i].mark;
        int guard = 0;
        while (c >= 0 && c < MAP_NBR_MARKS && rawMarks[c].row != 0xff && guard++ < MAP_NBR_MARKS)
        {
            const MarkRaw &r = rawMarks[c];
            MarkEntry e;
            e.rowAbs = base + r.row;
            e.col = r.xy >> 3;
            e.fineY = r.xy & 0x7;
            e.ent = r.ent;
            e.flags = r.flags;
            e.trigCol = r.lt >> 3;
            e.trigRowOffset = r.lt & 0x7;
            out.marks[i].push_back(e);
            c++;
        }
    }
    out.loaded = true;
    return out;
}

inline MarksData defaultMarks()
{
    return decodeMarksFromRaw(defaultSubmapsRaw, defaultMarksRaw);
}

inline bool loadXrickMarks(const fs::path &path, const ConnectionsData &conn, MarksData &out, std::string &err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "Could not open " + path.string(); return false; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (buf.empty()) { err = "File is empty or unreadable"; return false; }

    size_t off = 0, size = 0;
    if (!elf32_find_symbol_file_offset(buf, "map_submaps", off, size, err)) return false;
    if (size != (size_t)MAP_NBR_SUBMAPS * 8) { err = "map_submaps size mismatch"; return false; }
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

    size_t moff = 0, msize = 0;
    if (!elf32_find_symbol_file_offset(buf, "map_marks", moff, msize, err)) return false;
    if (msize != (size_t)MAP_NBR_MARKS * 5)
    {
        err = "map_marks has an unexpected size (" + std::to_string(msize) + " bytes) -- incompatible xrick build.";
        return false;
    }
    std::array<MarkRaw, MAP_NBR_MARKS> rawMarks;
    for (int i = 0; i < MAP_NBR_MARKS; i++)
        rawMarks[i] = {buf[moff + i*5+0], buf[moff + i*5+1], buf[moff + i*5+2], buf[moff + i*5+3], buf[moff + i*5+4]};

    (void)conn; // kept in the signature for symmetry/future use; submap geometry comes from the binary itself here
    out = decodeMarksFromRaw(rawSubmaps, rawMarks);
    err.clear();
    return true;
}

// Rebuilds the flat map_marks array (and each submap's `mark` start
// offset) from the per-submap lists in `data`, using `conn` for each
// submap's bnum (to convert absolute rows back to local bytes). Fails on
// out-of-range rows or if it doesn't fit the fixed 523-slot capacity.
inline bool repackMarks(MarksData &data, const ConnectionsData &conn, std::string &err)
{
    std::vector<MarkRaw> flat;
    flat.reserve(MAP_NBR_MARKS);
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
    {
        int base = submapStartRow(conn.submaps[i]);
        for (auto &e : data.marks[i])
        {
            int localRow = e.rowAbs - base;
            if (localRow < 0 || localRow > 255)
            {
                err = "Submap " + std::to_string(i) + ": sprite row " + std::to_string(e.rowAbs)
                    + " is too far from this submap's own rows to fit the original format.";
                return false;
            }
            if (e.col < 0 || e.col > 31)
            {
                err = "Submap " + std::to_string(i) + ": sprite column " + std::to_string(e.col) + " must be 0-31.";
                return false;
            }
            uint8_t xy = (uint8_t)(((e.col & 0x1f) << 3) | (e.fineY & 0x7));
            uint8_t lt = (uint8_t)(((e.trigCol & 0x1f) << 3) | (e.trigRowOffset & 0x7));
            flat.push_back(MarkRaw{(uint8_t)localRow, (uint8_t)e.ent, (uint8_t)e.flags, xy, lt});
        }
        flat.push_back(MarkRaw{0xff, 0, 0, 0, 0}); // end-of-chain marker
    }
    if ((int)flat.size() > MAP_NBR_MARKS)
    {
        err = "Too many sprites (" + std::to_string(flat.size()) + " slots needed) -- "
              "the original binary only has room for " + std::to_string(MAP_NBR_MARKS)
              + " total (across all submaps, including one end-marker per submap). Remove some sprites.";
        return false;
    }
    while ((int)flat.size() < MAP_NBR_MARKS) flat.push_back(MarkRaw{0xff, 0, 0, 0, 0});

    data.markStart.assign(MAP_NBR_SUBMAPS, 0);
    {
        int idx = 0;
        for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
        {
            data.markStart[i] = (uint16_t)idx;
            idx += (int)data.marks[i].size() + 1;
        }
    }

    data.packedMarks.assign(MAP_NBR_MARKS * 5, 0);
    for (int i = 0; i < MAP_NBR_MARKS; i++)
    {
        data.packedMarks[i * 5 + 0] = flat[i].row;
        data.packedMarks[i * 5 + 1] = flat[i].ent;
        data.packedMarks[i * 5 + 2] = flat[i].flags;
        data.packedMarks[i * 5 + 3] = flat[i].xy;
        data.packedMarks[i * 5 + 4] = flat[i].lt;
    }
    err.clear();
    return true;
}

// --- .map v4: level layout + screen connections + sprites ------------
//
// Format ("RKM4" magic): same header/body as v2, followed by:
//   per submap (submapCount times):
//     int32 markCount
//     markCount * { int32 rowAbs, int32 col, int32 fineY, int32 ent, int32 flags, int32 trigCol, int32 trigRowOffset }
//
// (v3/"RKM3" used a different, since-corrected sprite row/trigger encoding
// -- not supported; re-save from that version's editor build if you still
// have one.) Older "RKM2" (level + connections) and "RKMP" (level only)
// files still open fine; missing parts are simply left as they currently are.
struct MapFileHeaderV3 { char magic[4]; uint32_t bnumsCount; uint32_t submapCount; };

inline bool saveMapFileWithSprites(const fs::path &path, const ConnectionsData &conn, const MarksData &marks, std::string &err)
{
    FILE *f = std::fopen(path.string().c_str(), "wb");
    if (!f) { err = "Could not open file for writing"; return false; }
    MapFileHeaderV3 hdr{{'R', 'K', 'M', '4'}, (uint32_t)MAP_COUNT, (uint32_t)MAP_NBR_SUBMAPS};
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
    for (int i = 0; ok && i < MAP_NBR_SUBMAPS; i++)
    {
        int32_t markCount = (int32_t)marks.marks[i].size();
        ok = ok && std::fwrite(&markCount, 4, 1, f) == 1;
        for (auto &m : marks.marks[i])
        {
            int32_t vals[7] = {m.rowAbs, m.col, m.fineY, m.ent, m.flags, m.trigCol, m.trigRowOffset};
            ok = ok && std::fwrite(vals, 4, 7, f) == 7;
        }
    }
    std::fclose(f);
    if (!ok) { err = "Write error"; return false; }
    return true;
}

// Loads a .map file into `conn` and `marks`. Handles all three format
// generations (RKM4 / RKM2 / RKMP); parts absent from an older file are
// left untouched (whatever `conn`/`marks` already held on entry).
inline bool loadMapFileWithSprites(const fs::path &path, ConnectionsData &conn, MarksData &marks, std::string &err)
{
    std::ifstream probe(path, std::ios::binary);
    if (!probe) { err = "Could not open " + path.string(); return false; }
    char magic[4];
    probe.read(magic, 4);
    probe.close();

    if (std::memcmp(magic, "RKM4", 4) != 0)
        return loadMapFileWithConnections(path, conn, err); // handles RKM2 and RKMP too

    FILE *f = std::fopen(path.string().c_str(), "rb");
    if (!f) { err = "Could not open file"; return false; }
    MapFileHeaderV3 hdr{};
    bool ok = std::fread(&hdr, sizeof(hdr), 1, f) == 1
            && hdr.bnumsCount == (uint32_t)MAP_COUNT && hdr.submapCount == (uint32_t)MAP_NBR_SUBMAPS;
    if (!ok) { std::fclose(f); err = "Unsupported or mismatched .map file"; return false; }
    ok = std::fread(map_bnums, sizeof(int), MAP_COUNT, f) == (size_t)MAP_COUNT;
    if (!ok) { std::fclose(f); err = "Read error (level layout)"; return false; }

    ConnectionsData loadedConn;
    loadedConn.loaded = true;
    for (int i = 0; ok && i < MAP_NBR_SUBMAPS; i++)
    {
        int32_t page, bnum, mark, exitCount;
        ok = std::fread(&page, 4, 1, f) == 1 && std::fread(&bnum, 4, 1, f) == 1
          && std::fread(&mark, 4, 1, f) == 1 && std::fread(&exitCount, 4, 1, f) == 1;
        if (!ok) break;
        loadedConn.submaps[i] = {(int)page, (int)bnum, (int)mark};
        loadedConn.exits[i].clear();
        for (int e = 0; e < exitCount && ok; e++)
        {
            int32_t vals[4];
            ok = std::fread(vals, 4, 4, f) == 4;
            if (ok) loadedConn.exits[i].push_back(ConnectEntry{(int)vals[0], (int)vals[1], (int)vals[2], (int)vals[3]});
        }
    }
    if (!ok) { std::fclose(f); err = "Read error (screen connections)"; return false; }

    MarksData loadedMarks;
    loadedMarks.loaded = true;
    for (int i = 0; ok && i < MAP_NBR_SUBMAPS; i++)
    {
        int32_t markCount;
        ok = std::fread(&markCount, 4, 1, f) == 1;
        if (!ok) break;
        loadedMarks.marks[i].clear();
        for (int m = 0; m < markCount && ok; m++)
        {
            int32_t vals[7];
            ok = std::fread(vals, 4, 7, f) == 7;
            if (ok) loadedMarks.marks[i].push_back(MarkEntry{(int)vals[0], (int)vals[1], (int)vals[2], (int)vals[3], (int)vals[4], (int)vals[5], (int)vals[6]});
        }
    }
    std::fclose(f);
    if (!ok) { err = "Read error (sprites)"; return false; }

    conn = loadedConn;
    marks = loadedMarks;
    return true;
}

inline PatchResult patchXrickBinaryWithSprites(const fs::path &xrickPath, ConnectionsData &conn, MarksData &marks)
{
    std::string err;
    if (!repackConnections(conn, err))
    {
        PatchResult res; res.message = "Could not patch the screen connections: " + err; return res;
    }
    if (!repackMarks(marks, conn, err))
    {
        PatchResult res; res.message = "Could not patch the sprites: " + err; return res;
    }
    // repackConnections() already wrote conn.packedSubmaps with its own
    // `connect` field recomputed; patch in the `mark` field too before
    // it gets written to the binary.
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
    {
        uint16_t mark = marks.markStart[i];
        std::memcpy(&conn.packedSubmaps[i * 8 + 6], &mark, 2);
    }

    std::ifstream in(xrickPath, std::ios::binary);
    PatchResult res;
    if (!in) { res.message = "Could not open " + xrickPath.string(); return res; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (buf.empty()) { res.message = "File is empty or unreadable"; return res; }

    std::vector<uint8_t> bnumsBytes = map_bnums_as_bytes();
    if (!elf32_patch_symbol(buf, "map_bnums", bnumsBytes.data(), bnumsBytes.size(), err))
    { res.message = "Could not patch the level layout: " + err; return res; }
    if (!elf32_patch_symbol(buf, "map_submaps", conn.packedSubmaps.data(), conn.packedSubmaps.size(), err))
    { res.message = "Could not patch map_submaps: " + err; return res; }
    if (!elf32_patch_symbol(buf, "map_connect", conn.packedConnect.data(), conn.packedConnect.size(), err))
    { res.message = "Could not patch map_connect: " + err; return res; }
    if (!elf32_patch_symbol(buf, "map_marks", marks.packedMarks.data(), marks.packedMarks.size(), err))
    { res.message = "Could not patch map_marks: " + err; return res; }

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
    res.message = "Patched level written to " + outPath.string() + " (level layout + screen connections + sprites)";
    return res;
}
