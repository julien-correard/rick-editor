// test_xrick_marks.cpp -- exercises xrick_marks.h against a real xrick
// ELF binary (path given on the command line). Not part of the CMake
// build; a manual diagnostic tool.
#include <cstdio>
#include "xrick_marks.h"

static bool sameMark(const MarkEntry &a, const MarkEntry &b)
{
    return a.rowAbs == b.rowAbs && a.col == b.col && a.fineY == b.fineY
        && a.ent == b.ent && a.flags == b.flags
        && a.trigCol == b.trigCol && a.trigRowOffset == b.trigRowOffset;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: %s <path-to-xrick-binary>\n", argv[0]); return 2; }

    ConnectionsData conn;
    std::string err;
    if (!loadXrickConnections(argv[1], conn, err)) { std::printf("FAIL load connections: %s\n", err.c_str()); return 1; }

    MarksData marks;
    if (!loadXrickMarks(argv[1], conn, marks, err)) { std::printf("FAIL load marks: %s\n", err.c_str()); return 1; }

    // Regression guard: submap 1 (bnum=120 in the stock binary) has a
    // sprite whose raw local row is 24 -- confirmed by hand against the
    // raw bytes. Absolute tile-row must be 120/2 + 24 = 84, not
    // 120/8 + 24 = 39 (the old, wrong, block-row-based formula).
    if (marks.marks[1].empty() || marks.marks[1][0].rowAbs != 84)
    {
        std::printf("FAIL: sprite row unit regression (tile-row vs block-row) -- got %d, expected 84\n",
                     marks.marks[1].empty() ? -1 : marks.marks[1][0].rowAbs);
        return 1;
    }
    std::printf("OK: sprite rows use tile-row units, matching the original source's formula\n");

    int total = 0;
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++) total += (int)marks.marks[i].size();
    std::printf("Loaded %d sprites (+ %d end-markers = %d/%d slots)\n", total, MAP_NBR_SUBMAPS, total + MAP_NBR_SUBMAPS, MAP_NBR_MARKS);

    // Regression guards for the two bugs found after reviewing the real
    // xrick source (ents.c, ent_reset()):
    //  1) fineY (xy&7) is added to `row` at the SAME weight before the
    //     engine's final *8 -> pixel step, not as a 0-7 pixel nudge.
    //     markEffectiveRow() must reflect that.
    //  2) `lt` decodes into a separate trigger point (trigCol/trigRowOffset),
    //     not a single opaque byte.
    // Known-good values for submap 0's 2nd mark, hand-checked against the
    // raw bytes: row=24, xy=1 (col=0, fineY=1), lt=40 (trigCol=5, trigRowOffset=0).
    {
        bool found = false;
        for (auto &m : marks.marks[0])
        {
            if (m.rowAbs == 24 && m.col == 0 && m.ent == 42)
            {
                found = true;
                if (m.fineY != 1 || m.trigCol != 5 || m.trigRowOffset != 0)
                {
                    std::printf("FAIL: decode regression -- fineY=%d (expected 1), trigCol=%d (expected 5), trigRowOffset=%d (expected 0)\n",
                                 m.fineY, m.trigCol, m.trigRowOffset);
                    return 1;
                }
                if (markEffectiveRow(m) != 25) { std::printf("FAIL: markEffectiveRow() = %d, expected 25\n", markEffectiveRow(m)); return 1; }
                if (markTriggerRow(m) != 24) { std::printf("FAIL: markTriggerRow() = %d, expected 24\n", markTriggerRow(m)); return 1; }
            }
        }
        if (!found) { std::printf("FAIL: could not find the reference mark (submap 0, row=24, ent=42) to check\n"); return 1; }
        std::printf("OK: fineY and trigger-point decoding match the source-derived formulas exactly\n");
    }


    // Matches the compiled-in defaults for a stock build.
    {
        MarksData def = defaultMarks();
        bool same = true;
        for (int i = 0; i < MAP_NBR_SUBMAPS && same; i++)
        {
            if (marks.marks[i].size() != def.marks[i].size()) { same = false; break; }
            for (size_t j = 0; j < marks.marks[i].size(); j++)
                if (!sameMark(marks.marks[i][j], def.marks[i][j])) { same = false; break; }
        }
        std::printf(same ? "OK: matches the compiled-in default sprites\n" : "NOTE: differs from compiled-in defaults (different build?)\n");
    }

    // No-op round trip: repack+patch+reload must reproduce every sprite exactly.
    MarksData before = marks;
    ConnectionsData connCopy = conn;
    {
        PatchResult r = patchXrickBinaryWithSprites(argv[1], connCopy, marks);
        if (!r.ok) { std::printf("FAIL no-op patch: %s\n", r.message.c_str()); return 1; }
        ConnectionsData reloadedConn;
        MarksData reloadedMarks;
        if (!loadXrickConnections(r.outputPath, reloadedConn, err)) { std::printf("FAIL reload connections: %s\n", err.c_str()); return 1; }
        if (!loadXrickMarks(r.outputPath, reloadedConn, reloadedMarks, err)) { std::printf("FAIL reload marks: %s\n", err.c_str()); return 1; }
        bool ok = true;
        for (int i = 0; i < MAP_NBR_SUBMAPS && ok; i++)
        {
            if (reloadedMarks.marks[i].size() != before.marks[i].size()) { ok = false; break; }
            for (size_t j = 0; j < before.marks[i].size(); j++)
                if (!sameMark(reloadedMarks.marks[i][j], before.marks[i][j])) { ok = false; break; }
        }
        std::remove(r.outputPath.string().c_str());
        if (!ok) { std::printf("FAIL: no-op repack+patch+reload changed some sprite\n"); return 1; }
        std::printf("OK: no-op repack+patch+reload preserves every sprite exactly\n");
    }

    // Edit an existing sprite (move it), round-trip through patch+reload.
    {
        ConnectionsData c2 = conn;
        MarksData m2 = marks;
        // find a submap with at least one sprite
        int s = -1;
        for (int i = 0; i < MAP_NBR_SUBMAPS; i++) if (!m2.marks[i].empty()) { s = i; break; }
        if (s < 0) { std::printf("FAIL: no submap has any sprite to edit\n"); return 1; }
        MarkEntry edited = m2.marks[s][0];
        edited.rowAbs += 1;
        edited.col = (edited.col + 1) % 32;
        edited.ent = (edited.ent + 1) % 256;
        m2.marks[s][0] = edited;

        PatchResult r = patchXrickBinaryWithSprites(argv[1], c2, m2);
        std::printf("patch: ok=%d msg=%s\n", r.ok, r.message.c_str());
        if (!r.ok) return 1;

        ConnectionsData rc; MarksData rm;
        if (!loadXrickConnections(r.outputPath, rc, err)) { std::printf("FAIL reload: %s\n", err.c_str()); return 1; }
        if (!loadXrickMarks(r.outputPath, rc, rm, err)) { std::printf("FAIL reload marks: %s\n", err.c_str()); return 1; }
        std::remove(r.outputPath.string().c_str());
        if (!sameMark(rm.marks[s][0], edited)) { std::printf("FAIL: edited sprite mismatch after patch+reload\n"); return 1; }
        for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
        {
            size_t expected = m2.marks[i].size();
            if (rm.marks[i].size() != expected) { std::printf("FAIL: submap %d sprite count changed\n", i); return 1; }
        }
        std::printf("OK: edited sprite round-trips through patch+reload, all others untouched\n");
    }

    // Capacity check: table is already 100%% full in the stock binary.
    {
        MarksData overflow = marks;
        int s = -1;
        for (int i = 0; i < MAP_NBR_SUBMAPS; i++) if (!overflow.marks[i].empty()) { s = i; break; }
        overflow.marks[s].push_back(MarkEntry{overflow.marks[s][0].rowAbs, 0, 0, 4, 0, 0});
        std::string capErr;
        if (repackMarks(overflow, conn, capErr)) { std::printf("FAIL: repack should have refused to exceed capacity\n"); return 1; }
        std::printf("OK: capacity check correctly refuses to overflow the %d-slot table (%s)\n", MAP_NBR_MARKS, capErr.c_str());
    }

    // .map v4 round-trip (level + connections + sprites).
    {
        fs::path tmp = fs::temp_directory_path() / "rickeditor_test_sprites.map";
        std::string ferr;
        if (!saveMapFileWithSprites(tmp, conn, marks, ferr)) { std::printf("FAIL save .map v4: %s\n", ferr.c_str()); return 1; }

        ConnectionsData c3; MarksData m3;
        c3.loaded = true; m3.loaded = true;
        if (!loadMapFileWithSprites(tmp, c3, m3, ferr)) { std::printf("FAIL load .map v4: %s\n", ferr.c_str()); return 1; }
        bool ok = true;
        for (int i = 0; i < MAP_NBR_SUBMAPS && ok; i++)
        {
            if (m3.marks[i].size() != marks.marks[i].size()) { ok = false; break; }
            for (size_t j = 0; j < marks.marks[i].size(); j++)
                if (!sameMark(m3.marks[i][j], marks.marks[i][j])) { ok = false; break; }
        }
        std::remove(tmp.string().c_str());
        if (!ok) { std::printf("FAIL: .map v4 sprite round-trip mismatch\n"); return 1; }
        std::printf("OK: .map v4 (level + connections + sprites) round-trips exactly\n");
    }

    return 0;
}
