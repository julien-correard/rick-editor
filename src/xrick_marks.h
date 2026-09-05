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
//     flags: per-entity behavior flags, confirmed against the real
//            source (xrick/include/ents.h, xrick/src/e_them.c). NOT the
//            same byte as MAP_MARK_NACT, which is a bit of `ent`
//            (unrelated, not decoded here). Bits, from ents.h:
//              0x01 ENT_FLG_ONCE       run once only (don't loop/respawn)
//              0x02 ENT_FLG_STOPRICK   entity stops Rick (e.g. solid block)
//              0x04 ENT_FLG_LETHALR    lethal when restarting a loop
//              0x08 ENT_FLG_LETHALI    lethal from the moment it wakes up
//              0x10 ENT_FLG_TRIGBOMB   wakes up when hit by a bomb
//              0x20 ENT_FLG_TRIGBULLET wakes up when hit by a bullet
//              0x40 ENT_FLG_TRIGSTOP   wakes up when Rick does his "stop" move
//              0x80 ENT_FLG_TRIGRICK   wakes up when Rick walks into the trigger box
//            The TRIG* bits only matter for "type 3" entities (ent id
//            >= 0x18, see e_them_t3_action in e_them.c): those start
//            asleep and NEVER wake up unless at least one TRIG* bit is
//            set here -- e.g. a freshly-placed trap with flags=0 will
//            never fire, no matter where its trigger point is.
//     lt:    packed trigger info (trig_x / lat & trig_y), used by some
//            trap-like entities. See ENT_FLG_TRIG* above for when it's
//            actually consulted -- type "1a" walkers (see e_them.c)
//            instead reuse trig_x as a patrol-distance counter, not a
//            spatial trigger at all.
#pragma once

#include <array>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <algorithm>

#include "xrick_levels.h" // submapStartRow, ConnectionsData, find_symbol_file_offset / patch_symbol helpers, connections_default.h
#include "xrick_eflg.h"   // EflgData, repackEflg -- patched alongside sprites/connections below
#include "tiles.h"        // tile_t, tiles_data[][] -- persisted (banks 1/2) alongside eflg below
#include "sprites.h"      // sprite_t, sprites_data[], SPRITES_NBR_SPRITES -- persisted below too
#include "screens_text.h" // ImapText, SCREEN_IMAPTEXT_COUNT, encode/decode/patch helpers -- persisted below too

static const int MAP_NBR_MARKS = 0x20B; // 523

// Entity behavior flag bits (mark_t.flags) -- see the file header comment
// above and xrick/include/ents.h / e_them.c for the authoritative source.
static const int ENT_FLG_ONCE = 0x01;
static const int ENT_FLG_STOPRICK = 0x02;
static const int ENT_FLG_LETHALR = 0x04;
static const int ENT_FLG_LETHALI = 0x08;
static const int ENT_FLG_TRIGBOMB = 0x10;
static const int ENT_FLG_TRIGBULLET = 0x20;
static const int ENT_FLG_TRIGSTOP = 0x40;
static const int ENT_FLG_TRIGRICK = 0x80;

// Special case, confirmed in ents.c's entity-creation code
// (ent_reset()): for a type 1a/1b/2 entity (ent id 4-15, i.e. one that
// lands in the walker/climber slot pool, checked there as `e >= 0x09`
// on the SLOT index, not the mark's own type id), having ALL FOUR
// TRIG* bits set at once (flags == 0xF0 exactly) does something
// unrelated to their usual "wake up a sleeping trap" meaning for type-3
// entities: it makes `sprbase` -- normally the entity's own resting
// sprite -- get overwritten with its OWN `sni` (step_no_i) field
// instead, reinterpreted as a raw sprite index. sni is otherwise dead
// data for these types (their movement logic never reads it -- only
// type-3 traps' sleep/wake sequence does), so the original developers
// repurposed it as a place to stash a DIFFERENT sprite to switch to.
// The entity keeps rendering its own (type 1) sprite while airborne
// (`sprite` is set separately from `sprbase` at creation, and only
// recalculated from `sprbase` in e_them_t1_action2() on landing) --
// so visually, a walker configured this way keeps its own look while
// falling, then switches to whatever `sni` points at (typically
// another entity type's `spr`) the instant it touches the ground.
// The real xrick source itself calls this out as mysterious ("FIXME
// what is this? ... Why? What is the point?") and references the
// visible in-game example: "the falling guy on the right on submap 3:
// it changes when hitting the ground." This is the "type 1 morphs
// into type 2 on landing" mechanic.
static const int ENT_FLG_MORPH_TO_TYPE2 = ENT_FLG_TRIGBOMB | ENT_FLG_TRIGBULLET | ENT_FLG_TRIGSTOP | ENT_FLG_TRIGRICK; // 0xF0

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

// The real engine masks the raw row byte with `& 0xf8` before using it,
// for BOTH the entity's own y and its trigger's y (ents.c: `row & 0xf8`
// appears in both formulas) -- so only every 8th LOCAL tile-row (relative
// to the owning submap's own start row, not an absolute multiple) is a
// legal coarse `rowAbs`. Any leftover is meant to live in `fineY` (for
// the entity's own row) and `trigRowOffset` (for the trigger's row) --
// both 0-7, neither masked away -- so effectively any pair of absolute
// rows is still reachable, just not directly through `rowAbs` alone.
// Placing or editing a mark whose (rowAbs - base) isn't a multiple of 8
// will look fine in this editor's own preview (which doesn't replicate
// the masking) but gets silently rounded DOWN -- i.e. shifted to an
// EARLIER, physically HIGHER row on screen -- by the real, patched game
// at runtime, for BOTH the entity and its trigger.
//
// snapMarkRowToBase() folds the constraint in losslessly by moving the
// remainder into fineY *and* trigRowOffset together, so both the
// entity's own effective row (rowAbs+fineY) and its trigger's effective
// row (rowAbs+trigRowOffset) are preserved exactly. This must touch
// both fields together: an earlier version of this fix only compensated
// fineY, which kept the sprite's own drawn position correct but silently
// dragged the trigger along with the coarse row shift, uncompensated --
// exactly the "trap fires one block too high" bug this now avoids.
// Tries rounding the coarse row down first, then up, whichever keeps
// both resulting fine values in [0,7]; in the (very rare) case a mark's
// own fineY and trigRowOffset straddle an 8-row boundary by the maximum
// possible 7-row spread, perfect preservation of both isn't always
// possible with a single shared coarse row -- when that happens this
// keeps the entity's own row exact and clamps the trigger's.
static inline int snapMarkRowToBase(int rowAbs, int base, int fineYOld, int trigRowOffsetOld,
                                     int &fineYOut, int &trigRowOffsetOut)
{
    int local = rowAbs - base;
    if (local < 0) local = 0;
    int remFloor = local % 8;
    int coarseFloor = local - remFloor;
    int fineFloor = fineYOld + remFloor;
    int trigFloor = trigRowOffsetOld + remFloor;
    if (fineFloor <= 7 && trigFloor <= 7)
    {
        fineYOut = fineFloor;
        trigRowOffsetOut = trigFloor;
        return base + coarseFloor;
    }
    int coarseCeil = coarseFloor + 8;
    int remCeil = remFloor - 8; // negative
    int fineCeil = fineYOld + remCeil;
    int trigCeil = trigRowOffsetOld + remCeil;
    if (fineCeil >= 0 && trigCeil >= 0)
    {
        fineYOut = fineCeil;
        trigRowOffsetOut = trigCeil;
        return base + coarseCeil;
    }
    // Rare straddling case: keep the entity's own row exact, clamp the trigger.
    fineYOut = std::clamp(fineFloor, 0, 7);
    trigRowOffsetOut = std::clamp(trigFloor, 0, 7);
    return base + coarseFloor;
}
// Convenience overload for call sites that only care about the entity's
// own row (e.g. fresh placement, where trigRowOffset is still 0 and thus
// trivially preserved by either branch above).
static inline int snapMarkRowToBase(int rowAbs, int base, int &fineYOut)
{
    int trigDummy = 0, trigOut;
    return snapMarkRowToBase(rowAbs, base, 0, trigDummy, fineYOut, trigOut);
}

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
    if (!find_symbol_file_offset(buf, "map_submaps", off, size, err)) return false;
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
    if (!find_symbol_file_offset(buf, "map_marks", moff, msize, err)) return false;
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
        // The real engine's activation scan (ents.c's ent_actvis()) is a
        // simple forward-only linear scan over each submap's mark chain
        // that assumes ascending `row` and never backtracks (confirmed in
        // the source: "go through the list ... marks being ordered by row
        // number", with the scan index carried forward between calls).
        // Freshly placed sprites get appended to the end of this list
        // (insertion order), which is NOT necessarily row order once
        // there's already a lower-row mark ahead of it -- an out-of-order
        // entry can end up permanently skipped by that scan, never
        // spawning at all. Sort by row (stable, so same-row entries keep
        // their relative order) right before flattening to the raw
        // format, so the file always matches what the real engine needs
        // regardless of the order things were placed/edited in this
        // editor.
        std::stable_sort(data.marks[i].begin(), data.marks[i].end(),
                          [](const MarkEntry &a, const MarkEntry &b) { return a.rowAbs < b.rowAbs; });
        for (auto &e : data.marks[i])
        {
            // Defensive: the real engine only respects rows that are a
            // multiple of 8 LOCAL tile-rows from the submap's start (see
            // snapMarkRowToBase() above) -- fold any remainder into
            // fineY *and* trigRowOffset together here, so a mark
            // edited/placed off that grid (e.g. by older data, or a
            // future UI bug) still ends up patched exactly where it
            // visually sits in this editor -- both the entity's own row
            // and its trigger's row -- instead of silently drifting once
            // the real game masks the raw byte.
            int newFineY, newTrigRowOffset;
            int snappedRowAbs = snapMarkRowToBase(e.rowAbs, base, e.fineY, e.trigRowOffset, newFineY, newTrigRowOffset);
            if (snappedRowAbs != e.rowAbs)
            {
                e.fineY = newFineY;
                e.trigRowOffset = newTrigRowOffset;
                e.rowAbs = snappedRowAbs;
            }
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

// --- .map v4/v5: level layout + screen connections + sprites (+ tile hazards) ---
//
// Format ("RKM4" magic): same header/body as v2, followed by:
//   per submap (submapCount times):
//     int32 markCount
//     markCount * { int32 rowAbs, int32 col, int32 fineY, int32 ent, int32 flags, int32 trigCol, int32 trigRowOffset }
//
// Format ("RKM5" magic): same as RKM4, followed by:
//   2 * 256 bytes: per-tile environment/hazard flags (bank 1, then bank 2 -- see xrick_eflg.h)
//
// Format ("RKM6" magic): same as RKM5, followed by:
//   2 * 256 * sizeof(tile_t) bytes: tile GRAPHICS (bank 1, then bank 2 --
//   see tile_import.h and the Tile Editor window), raw tiles_data[bank][]
//   memory dump, same order/format as the compiled-in data. Bank 0
//   (unused padding, never shown/edited) is deliberately not stored --
//   nothing in the editor ever changes it, so re-deriving it from the
//   compiled-in default on load is always correct.
//
// Format ("RKM7" magic): same as RKM6, followed by:
//   0x100 * 16 * sizeof(int) bytes: map_blocks (which tile goes in each
//   of the 16 cells of each of the 256 blocks -- see the Block Editor
//   window), raw in-memory dump, same order/type as this editor's own
//   `int map_blocks[0x100][16]` (mapdata.h). Shared by both tile banks
//   (a block's tile *positions* don't depend on which bank's graphics
//   are used to render it), so there's only one copy to store, unlike
//   tile graphics/hazard flags above.
//
// Format ("RKM8" magic): same as RKM7, followed by:
//   SPRITES_NBR_SPRITES * sizeof(sprite_t) bytes: sprite GRAPHICS (see
//   sprite_import.h and the Sprite Editor window), raw sprites_data[]
//   memory dump. One flat table, no bank split (unlike tiles).
//
// Format ("RKM9" magic): same as RKM8, followed by, for each of the 5
//   between-levels intro texts (see the Text Editor window and
//   screens_text.h), in SCREEN_IMAPTEXT_LABELS[]/game_map order:
//     int32 rowCount, then for each row: int32 textLen, `textLen` raw
//     bytes (the row's text, real spaces -- NOT `@`), uint8 blankLineAfter.
//   Unlike every fixed-size table above, text length varies per edit
//   (it's not a raw memory dump of a fixed C array), hence the explicit
//   length prefixes -- this is our own format's freedom; the ELF PATCH
//   path below is the one still constrained to the original fixed byte
//   count per text, since it overwrites an existing symbol in place.
//
// Format ("RKMA" magic -- hex "A" = 10; magic is a fixed 4-byte field,
//   no room for two-digit "RKM10"): same as RKM9, followed by:
//   256 * sizeof(tile_t) bytes: bank 0's tile GRAPHICS, raw
//   tiles_data[0][] memory dump. RKM6 deliberately skipped bank 0
//   ("unused padding, never shown/edited") -- no longer true once the
//   Tile Editor exposed bank 0 for editing the font (Text Editor) and
//   the cutscene decor (drawcenter() in the original scr_imap.c, see
//   screens_text.h's SCREEN_IMAP_DECOR_* -- both live in bank 0), so
//   this format bump closes the gap: bank 0 edits would otherwise
//   silently vanish on save/reload despite already surviving the ELF
//   patch path (which always covered all 3 banks via `sizeof(tiles_data)`).
//
// (v3/"RKM3" used a different, since-corrected sprite row/trigger encoding
// -- not supported; re-save from that version's editor build if you still
// have one.) Older "RKM2" (level + connections) and "RKMP" (level only)
// files still open fine; missing parts (including tile hazards from any
// file older than RKM5, tile graphics from any file older than RKM6,
// block composition from any file older than RKM7, sprite graphics from
// any file older than RKM8, intro text from any file older than RKM9,
// and bank-0 tile graphics -- font/decor -- from any file older than
// RKMA) are simply left as they currently are.
struct MapFileHeaderV3 { char magic[4]; uint32_t bnumsCount; uint32_t submapCount; };

// Magic ladder (see the format doc below):
//   RKMB -- the stock/legacy full format (ids 0-73; 2 game-tile pages).
//   RKMC -- "Rick Ultra Xpanded" format. Same body ordering as RKMB, but
//           the trailing tile/hazard/block sections carry FOUR game-tile
//           pages (banks 1-4, 1024 tiles; 1024 shared blocks) instead of
//           two, and it marks the map as using the extended entity ids
//           (74-76 and whatever follows) that only the modified RUxP
//           xrick executable understands. A legacy xrick cannot be
//           patched with an RKMC map.
inline bool saveMapFileWithSprites(const fs::path &path, const ConnectionsData &conn, const MarksData &marks, const EflgData &eflg, const std::array<ImapText, SCREEN_IMAPTEXT_COUNT> &texts, bool ruxp, std::string &err)
{
    FILE *f = std::fopen(path.string().c_str(), "wb");
    if (!f) { err = "Could not open file for writing"; return false; }
    MapFileHeaderV3 hdr{ {'R', 'K', 'M', ruxp ? 'D' : 'B'}, (uint32_t)MAP_COUNT, (uint32_t)MAP_NBR_SUBMAPS};
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
            if (ruxp)
            {
                // RUxF (RKMD): {dir, rowAbs(exit row), targetSubmap, targetRowAbs, col}.
                int32_t vals[5] = {e.dir, e.rowAbs, e.targetSubmap, e.targetRowAbs, e.col};
                ok = ok && std::fwrite(vals, 4, 5, f) == 5;
            }
            else
            {
                // Legacy (RKMB): {dir, rowAbs, targetSubmap, targetRowAbs}.
                int32_t vals[4] = {e.dir, e.rowAbs, e.targetSubmap, e.targetRowAbs};
                ok = ok && std::fwrite(vals, 4, 4, f) == 4;
            }
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
    // RUxF (RKMC) persists all 4 game-tile pages (hazard flags, tiles and
    // blocks); legacy persists the 2 stock pages. Bank 0 is stored once,
    // separately, below.
    const int pages = ruxp ? 4 : 2;
    for (int p = 0; ok && p < pages; p++)
        ok = std::fwrite(eflg.bank[p].data(), 1, 256, f) == 256;
    for (int p = 1; ok && p <= pages; p++)
        ok = std::fwrite(tiles_data[p], sizeof(tile_t), 0x100, f) == 0x100;
    ok = ok && std::fwrite(map_blocks, sizeof(int), ruxp ? 0x400 * 16 : 0x100 * 16, f) == (size_t)(ruxp ? 0x400 * 16 : 0x100 * 16);
    ok = ok && std::fwrite(sprites_data, sizeof(sprite_t), SPRITES_NBR_SPRITES, f) == (size_t)SPRITES_NBR_SPRITES;
    for (int i = 0; ok && i < SCREEN_IMAPTEXT_COUNT; i++)
    {
        int32_t rowCount = (int32_t)texts[i].rows.size();
        ok = ok && std::fwrite(&rowCount, 4, 1, f) == 1;
        for (auto &row : texts[i].rows)
        {
            int32_t textLen = (int32_t)row.text.size();
            uint8_t blank = row.blankLineAfter ? 1 : 0;
            ok = ok && std::fwrite(&textLen, 4, 1, f) == 1
                    && std::fwrite(row.text.data(), 1, row.text.size(), f) == row.text.size()
                    && std::fwrite(&blank, 1, 1, f) == 1;
        }
    }
    ok = ok && std::fwrite(tiles_data[0], sizeof(tile_t), 0x100, f) == 0x100;
    if (ok)
    {
        for (int i = 0; i < MAP_NBR_MAPS; i++)
        {
            int sm = conn.mapStarts[i].submap;
            int base = (sm >= 0 && sm < MAP_NBR_SUBMAPS) ? submapStartRow(conn.submaps[sm]) : 0;
            int rawRow = conn.mapStarts[i].row - base;
            int32_t vals[4] = {conn.mapStarts[i].x, conn.mapStarts[i].y, rawRow, conn.mapStarts[i].submap};
            ok = ok && std::fwrite(vals, 4, 4, f) == 4;
        }
    }
    std::fclose(f);
    if (!ok) { err = "Write error"; return false; }
    return true;
}

// Loads a .map file into `conn`, `marks`, and `eflg`. Handles all format
// generations (RKMC / RKMB / RKMA / RKM9 / ... / RKMP); parts absent from
// an older file are left untouched (whatever was passed in on entry).
// `ruxp` (if non-null) is set true iff the file was saved in the
// "Rick Ultra Xpanded" format (magic RKMC), i.e. it may use entity ids
// beyond 73 -- only the modified RUxP xrick executable understands those.
inline bool loadMapFileWithSprites(const fs::path &path, ConnectionsData &conn, MarksData &marks, EflgData &eflg, std::array<ImapText, SCREEN_IMAPTEXT_COUNT> &texts, std::string &err, bool *ruxp = nullptr)
{
    std::ifstream probe(path, std::ios::binary);
    if (!probe) { err = "Could not open " + path.string(); return false; }
    char magic[4];
    probe.read(magic, 4);
    probe.close();

    if (ruxp) *ruxp = std::memcmp(magic, "RKMC", 4) == 0 || std::memcmp(magic, "RKMD", 4) == 0;
    // RUxP formats: RKMC (Rick Ultra Xpanded, legacy connector layout) and
    // RKMD (the current RUxF connector layout: right/left transitions with an
    // arrival column, no row trigger). Both are supersets of RKMB -- same body,
    // but each section is read identically, so just treat their trailing
    // sections as present (identical section flags to RKMB).
    bool isRkx = std::memcmp(magic, "RKMC", 4) == 0 || std::memcmp(magic, "RKMD", 4) == 0;
    bool isRuxfConn = std::memcmp(magic, "RKMD", 4) == 0;
    bool hasBank0Tiles = isRkx || std::memcmp(magic, "RKMB", 4) == 0 || std::memcmp(magic, "RKMA", 4) == 0;
    bool hasMapStarts = isRkx || std::memcmp(magic, "RKMB", 4) == 0;
    bool hasTexts = isRkx || hasBank0Tiles || std::memcmp(magic, "RKM9", 4) == 0;
    bool hasSprites = isRkx || hasTexts || std::memcmp(magic, "RKM8", 4) == 0;
    bool hasBlocks = isRkx || hasSprites || std::memcmp(magic, "RKM7", 4) == 0;
    bool hasTiles = isRkx || hasBlocks || std::memcmp(magic, "RKM6", 4) == 0;
    bool hasEflg = isRkx || hasTiles || std::memcmp(magic, "RKM5", 4) == 0;
    if (!hasEflg && std::memcmp(magic, "RKM4", 4) != 0)
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
            if (isRuxfConn)
            {
                int32_t vals[5];
                ok = std::fread(vals, 4, 5, f) == 5;
                if (ok) loadedConn.exits[i].push_back(ConnectEntry{vals[0], vals[1], vals[2], vals[3], vals[4]});
            }
            else
            {
                int32_t vals[4];
                ok = std::fread(vals, 4, 4, f) == 4;
                if (ok) loadedConn.exits[i].push_back(ConnectEntry{vals[0], vals[1], vals[2], vals[3], 0});
            }
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
            if (ok)
            {
                MarkEntry me{(int)vals[0], (int)vals[1], (int)vals[2], (int)vals[3], (int)vals[4], (int)vals[5], (int)vals[6]};
                me.trigRowOffset = std::clamp(me.trigRowOffset, 0, 7);
                me.fineY = std::clamp(me.fineY, 0, 7);
                loadedMarks.marks[i].push_back(me);
            }
        }
    }
    if (!ok) { std::fclose(f); err = "Read error (sprites)"; return false; }

    EflgData loadedEflg = eflg; // keep whatever was there if this file predates RKM5
    if (hasEflg)
    {
        const int pages = isRkx ? 4 : 2;
        ok = true;
        for (int p = 0; p < pages; p++)
            ok = ok && std::fread(loadedEflg.bank[p].data(), 1, 256, f) == 256;
        if (!ok) { std::fclose(f); err = "Read error (tile hazard flags)"; return false; }
    }
    if (hasTiles)
    {
        // Bank 0 isn't stored for game pages (see the format comment
        // above); legacy stores banks 1-2, RUxF stores banks 1-4. Read
        // into the live global array directly (same convention as
        // map_bnums above): the caller is responsible for rebuilding the
        // tile/block atlas textures afterwards (needs the SDL renderer,
        // not available in this header).
        const int pages = isRkx ? 4 : 2;
        ok = true;
        for (int p = 1; p <= pages; p++)
            ok = ok && std::fread(tiles_data[p], sizeof(tile_t), 0x100, f) == 0x100;
        if (!ok) { std::fclose(f); err = "Read error (tile graphics)"; return false; }
    }
    if (hasBlocks)
    {
        // Same direct-into-live-global convention as map_bnums and
        // tiles_data above -- caller rebuilds the block atlas texture
        // afterwards (needs the SDL renderer).
        const int blockCount = isRkx ? 0x400 : 0x100;
        ok = std::fread(map_blocks, sizeof(int), blockCount * 16, f) == (size_t)(blockCount * 16);
        if (!ok) { std::fclose(f); err = "Read error (block composition)"; return false; }
        // A block cell is an ABSOLUTE tile index (0-1023 in RUxF, 0-255 in
        // legacy). If any cell is out of range the file's block section is
        // corrupt (a partial/legacy load has written sprite/tile graphics in
        // place of block compositions). Refuse it now with a clear error
        // rather than silently clobbering the live map_blocks, which would
        // be propagated verbatim into the next save as a corrupt map.
        const int maxTile = isRkx ? 0x400 : 0x100;
        int badCells = 0;
        for (int b = 0; b < blockCount; b++)
            for (int i = 0; i < 16; i++)
                if ((unsigned)map_blocks[b][i] >= (unsigned)maxTile) badCells++;
        if (badCells)
        {
            std::fclose(f);
            err = "Corrupt map: " + std::to_string(badCells) +
                  " out-of-range block cell(s) (block cells must be tile indices, got sprite/graphics data). File appears to have been saved from a failed/corrupt load.";
            return false;
        }
    }
    std::array<ImapText, SCREEN_IMAPTEXT_COUNT> loadedTexts = texts; // keep whatever was there if this file predates RKM9
    if (hasSprites)
    {
        // Same direct-into-live-global convention as tiles_data/
        // map_blocks above -- caller rebuilds the sprite atlas texture
        // afterwards (needs the SDL renderer).
        //
        // The number of sprites actually stored in the file varies with the
        // build that produced it (213 before the RUxF life bonus brought the
        // table up to 214): nothing in the header records it. Detect it by
        // scanning candidate counts -- reading that many sprites must leave
        // the self-describing tail (intro texts + bank-0 tiles + map start
        // positions) cleanly parseable. The current count is tried first, so
        // current-format files take the fast path.
        long spritesPos = std::ftell(f);
        bool tailOk = false;
        for (int sprites = SPRITES_NBR_SPRITES; sprites >= 0 && !tailOk; sprites--)
        {
            if (std::fseek(f, spritesPos, SEEK_SET) != 0) break;
            if (std::fread(sprites_data, sizeof(sprite_t), sprites, f) != (size_t)sprites) continue;

            std::array<ImapText, SCREEN_IMAPTEXT_COUNT> candTexts;
            bool candOk = true;
            if (hasTexts)
            {
                for (int i = 0; candOk && i < SCREEN_IMAPTEXT_COUNT; i++)
                {
                    int32_t rowCount;
                    candOk = std::fread(&rowCount, 4, 1, f) == 1 && rowCount >= 0 && rowCount <= 100;
                    if (!candOk) break;
                    ImapText t;
                    for (int r = 0; r < rowCount && candOk; r++)
                    {
                        int32_t textLen;
                        uint8_t blank = 0;
                        candOk = std::fread(&textLen, 4, 1, f) == 1
                              && textLen >= 0 && textLen <= 2048;
                        if (!candOk) break;
                        std::string s((size_t)textLen, '\0');
                        candOk = (textLen == 0)
                              || (std::fread(&s[0], 1, (size_t)textLen, f) == (size_t)textLen);
                        candOk = candOk && std::fread(&blank, 1, 1, f) == 1;
                        if (candOk) t.rows.push_back(ImapTextRow{s, blank != 0});
                    }
                    if (candOk) candTexts[i] = t;
                }
            }
            if (candOk && hasBank0Tiles)
                candOk = std::fread(tiles_data[0], sizeof(tile_t), 0x100, f) == 0x100;
            if (candOk && hasMapStarts)
            {
                for (int i = 0; i < MAP_NBR_MAPS && candOk; i++)
                {
                    int32_t vals[4];
                    candOk = std::fread(vals, 4, 4, f) == 4;
                    if (candOk)
                    {
                        int sm = (int)vals[3];
                        int base = (sm >= 0 && sm < MAP_NBR_SUBMAPS) ? submapStartRow(loadedConn.submaps[sm]) : 0;
                        loadedConn.mapStarts[i] = {(int)vals[0], (int)vals[1], (int)vals[2] + base, sm};
                    }
                }
            }
            if (candOk)
            {
                loadedTexts = candTexts;
                tailOk = true;
                if (sprites < SPRITES_NBR_SPRITES)
                    std::memset(sprites_data[sprites], 0, sizeof(sprite_t) * (size_t)(SPRITES_NBR_SPRITES - sprites));
            }
        }
        if (!tailOk)
        {
            std::fclose(f);
            err = "Read error (sprite graphics)";
            return false;
        }
    }
    std::fclose(f);

    conn = loadedConn;
    marks = loadedMarks;
    eflg = loadedEflg;
    texts = loadedTexts;
    return true;
}

inline PatchResult patchXrickBinaryWithSprites(
    const fs::path &xrickPath, ConnectionsData &conn, MarksData &marks,
    const EflgData &eflg, const std::array<ImapText, SCREEN_IMAPTEXT_COUNT> &texts,
    const std::vector<std::pair<const char*, size_t>> &textSlots = {},
    const std::vector<std::vector<uint8_t>> &textEncoded = {})
{
    std::string err;
    std::ifstream in(xrickPath, std::ios::binary);
    PatchResult res;
    if (!in) { res.message = "Could not open " + xrickPath.string(); return res; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (buf.empty()) { res.message = "File is empty or unreadable"; return res; }

    // Detect whether this is an RUxF build (expanded engine) or a legacy
    // one. RUxF uses a flat U8 map_eflg[0x400] instead of the legacy
    // RLE-compressed map_eflg_c, and U16 map_bnums/map_blocks over the
    // unified 1024-block / 5-bank space. The legacy build has no flat
    // map_eflg symbol and no 0x400-sized hazard table.
    bool ruxfBinary = false;
    {
        size_t eo = 0, esize = 0;
        if (find_symbol_file_offset(buf, "map_eflg", eo, esize, err) && esize == 0x400)
            ruxfBinary = true;
    }

    if (!repackConnections(conn, err, ruxfBinary))
    {
        res.message = "Could not patch the screen connections: " + err; return res;
    }
    if (!repackMarks(marks, conn, err))
    {
        res.message = "Could not patch the sprites: " + err; return res;
    }
    // repackConnections() already wrote conn.packedSubmaps with its own
    // `connect` field recomputed; patch in the `mark` field too before
    // it gets written to the binary.
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
    {
        uint16_t mark = marks.markStart[i];
        std::memcpy(&conn.packedSubmaps[i * 8 + 6], &mark, 2);
    }

    std::vector<uint8_t> bnumsBytes = ruxfBinary ? map_bnums_as_ruxf_bytes() : map_bnums_as_bytes();
    if (!patch_symbol(buf, "map_bnums", bnumsBytes.data(), bnumsBytes.size(), err))
    { res.message = "Could not patch the level layout: " + err; return res; }
    if (!patch_symbol(buf, "map_submaps", conn.packedSubmaps.data(), conn.packedSubmaps.size(), err))
    { res.message = "Could not patch map_submaps: " + err; return res; }
    if (!patch_symbol(buf, "map_connect", conn.packedConnect.data(), conn.packedConnect.size(), err))
    { res.message = "Could not patch map_connect: " + err; return res; }
    // Patch map_maps (per-map start positions). Entry layout:
    //   {U16 x, U16 y, U16 row, U16 submap, char *tune}
    // Entry size varies: 12 bytes on ELF32 (4-byte pointer), 16 bytes on
    // ELF64 (8-byte pointer).  The xrick source always defines 5 entries;
    // we only patch the first MAP_NBR_MAPS and preserve the rest.
    {
        size_t moff = 0, msize = 0;
        if (find_symbol_file_offset(buf, "map_maps", moff, msize, err))
        {
            static const int ORIGINAL_MAP_COUNT = 5;
            size_t entrySize = (msize >= ORIGINAL_MAP_COUNT * 12 && msize % ORIGINAL_MAP_COUNT == 0)
                ? msize / ORIGINAL_MAP_COUNT : 0;
            if (entrySize >= 12 && msize >= (size_t)MAP_NBR_MAPS * entrySize)
            {
                size_t ptrSize = entrySize - 8; // tune pointer size (4 or 8)
                std::vector<uint8_t> patched(MAP_NBR_MAPS * entrySize);
                for (int i = 0; i < MAP_NBR_MAPS; i++)
                {
                    uint16_t x = (uint16_t)conn.mapStarts[i].x;
                    uint16_t y = (uint16_t)conn.mapStarts[i].y;
                    int sm = conn.mapStarts[i].submap;
                    int base = (sm >= 0 && sm < MAP_NBR_SUBMAPS) ? submapStartRow(conn.submaps[sm]) : 0;
                    int rawRow = conn.mapStarts[i].row - base;
                    rawRow = (rawRow / 4) * 4;
                    if (rawRow < 0) rawRow = 0;
                    if (rawRow > 255) rawRow = 255;
                    uint16_t row = (uint16_t)rawRow;
                    uint16_t submap = (uint16_t)conn.mapStarts[i].submap;
                    std::memcpy(&patched[i * entrySize + 0], &x, 2);
                    std::memcpy(&patched[i * entrySize + 2], &y, 2);
                    std::memcpy(&patched[i * entrySize + 4], &row, 2);
                    std::memcpy(&patched[i * entrySize + 6], &submap, 2);
                    // Preserve existing tune pointer
                    std::memcpy(&patched[i * entrySize + 8], &buf[moff + i * entrySize + 8], ptrSize);
                }
                std::memcpy(buf.data() + moff, patched.data(), MAP_NBR_MAPS * entrySize);
            }
        }
    }
    if (!patch_symbol(buf, "map_marks", marks.packedMarks.data(), marks.packedMarks.size(), err))
    { res.message = "Could not patch map_marks: " + err; return res; }
    if (ruxfBinary)
    {
        std::vector<uint8_t> eflgFlat = map_eflg_as_ruxf_bytes(eflg);
        if (!patch_symbol(buf, "map_eflg", eflgFlat.data(), eflgFlat.size(), err))
        { res.message = "Could not patch map_eflg (tile hazard flags): " + err; return res; }
    }
    else
    {
        uint8_t eflgPacked[MAP_NBR_EFLGC];
        if (!repackEflg(eflg, eflgPacked, err))
        { res.message = "Could not patch the tile hazard flags: " + err; return res; }
        if (!patch_symbol(buf, "map_eflg_c", eflgPacked, MAP_NBR_EFLGC, err))
        { res.message = "Could not patch map_eflg_c (tile hazard flags): " + err; return res; }
    }
    if (!patch_symbol(buf, "tiles_data", tiles_data, sizeof(tiles_data), err))
    { res.message = "Could not patch tiles_data (tile graphics): " + err; return res; }
    std::vector<uint8_t> blocksBytes = ruxfBinary ? map_blocks_as_ruxf_bytes() : map_blocks_as_bytes();
    if (!patch_symbol(buf, "map_blocks", blocksBytes.data(), blocksBytes.size(), err))
    { res.message = "Could not patch map_blocks (block composition): " + err; return res; }
    if (!patch_symbol(buf, "sprites_data", sprites_data, sizeof(sprites_data), err))
    { res.message = "Could not patch sprites_data (sprite graphics): " + err; return res; }
    for (int i = 0; i < SCREEN_IMAPTEXT_COUNT; i++)
    {
        size_t off = 0, symSize = 0;
        if (!find_symbol_file_offset(buf, SCREEN_IMAPTEXT_SYMBOLS[i], off, symSize, err))
        { res.message = "Could not patch " + std::string(SCREEN_IMAPTEXT_SYMBOLS[i]) + " (intro text): " + err; return res; }
        std::vector<uint8_t> textBytes;
        if (!encodeImapTextPadded(texts[i], symSize, textBytes, err))
        { res.message = "Could not patch " + std::string(SCREEN_IMAPTEXT_SYMBOLS[i])
            + " (" + SCREEN_IMAPTEXT_LABELS[i] + "): " + err; return res; }
        if (!patch_symbol(buf, SCREEN_IMAPTEXT_SYMBOLS[i], textBytes.data(), textBytes.size(), err))
        { res.message = "Could not patch " + std::string(SCREEN_IMAPTEXT_SYMBOLS[i]) + " (intro text): " + err; return res; }
    }
    // Patch additional ASCII text screens (e.g. copyright, game over, pause)
    for (size_t i = 0; i < textSlots.size() && i < textEncoded.size(); i++)
    {
        size_t off = 0, symSize = 0;
        std::string lerr;
        if (!find_symbol_file_offset(buf, textSlots[i].first, off, symSize, lerr))
            continue; // skip if symbol not found
        std::vector<uint8_t> padded = textEncoded[i];
        padded.resize(symSize, 0x00);
        std::memcpy(buf.data() + off, padded.data(), symSize);
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
    res.message = "Patched level written to " + outPath.string() + " (level layout + screen connections + sprites + tile hazard flags + tile graphics + block composition + sprite graphics + intro text)";
    return res;
}
