// test_xrick_levels.cpp -- exercises xrick_levels.h against a real xrick
// ELF binary (path given on the command line). Not part of the CMake
// build; a manual diagnostic tool.
#include <cstdio>
#include "xrick_levels.h"

static bool sameExit(const ConnectEntry &a, const ConnectEntry &b)
{
    return a.dir == b.dir && a.rowAbs == b.rowAbs
        && a.targetSubmap == b.targetSubmap
        && (a.targetSubmap == SUBMAP_END_OF_LEVEL || a.targetRowAbs == b.targetRowAbs);
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: %s <path-to-xrick-binary>\n", argv[0]); return 2; }

    ConnectionsData conn;
    std::string err;
    if (!loadXrickConnections(argv[1], conn, err)) { std::printf("FAIL load: %s\n", err.c_str()); return 1; }

    // Regression guard for the tile-row vs block-row unit bug (fixed after
    // reviewing the real xrick source: maps.c documents map_frow, and by
    // extension connect_t's rows, as TILE-row scaled, not block-row).
    // submap 1's bnum is 120 in the stock binary -> tile-row start must be
    // 60 (120/2), not 15 (120/8).
    if (submapStartRow(conn.submaps[1]) != conn.submaps[1].bnum / 2)
    {
        std::printf("FAIL: submapStartRow unit regression (tile-row vs block-row)\n");
        return 1;
    }
    std::printf("OK: submapStartRow uses tile-row units (bnum/2), not block-row (bnum/8)\n");

    int totalExits = 0;
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++) totalExits += (int)conn.exits[i].size();
    std::printf("Loaded %d submaps, %d total exits (+ %d end-markers = %d/%d slots)\n",
                MAP_NBR_SUBMAPS, totalExits, MAP_NBR_SUBMAPS, totalExits + MAP_NBR_SUBMAPS, MAP_NBR_CONNECT);

    // Sanity check: absolute rows must always be >= the owning submap's
    // own start row (local can't be negative straight out of a valid load).
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
    {
        int base = submapStartRow(conn.submaps[i]);
        for (auto &e : conn.exits[i])
            if (e.rowAbs < base) { std::printf("FAIL: submap %d has an exit row below its own start row\n", i); return 1; }
    }
    std::printf("OK: all loaded rows are consistent absolute values\n");

    // Compare against the compiled-in defaults (connections_default.h was
    // generated from a stock xrick binary -- if the binary under test is
    // the same stock build, decoding both must agree exactly).
    {
        ConnectionsData def = defaultConnections();
        bool same = true;
        for (int i = 0; i < MAP_NBR_SUBMAPS && same; i++)
        {
            if (conn.submaps[i].page != def.submaps[i].page || conn.submaps[i].bnum != def.submaps[i].bnum) same = false;
            if (conn.exits[i].size() != def.exits[i].size()) same = false;
            else for (size_t j = 0; j < conn.exits[i].size(); j++)
                if (!sameExit(conn.exits[i][j], def.exits[i][j])) { same = false; break; }
        }
        std::printf(same ? "OK: matches the compiled-in default connections\n"
                          : "NOTE: differs from the compiled-in defaults (different xrick build?)\n");
    }

    // Round-trip #1: repack WITHOUT any edits, patch, reload, check every
    // submap's exits come back identical (absolute rows, so this is now a
    // straightforward equality check, no local/global ambiguity).
    ConnectionsData beforeEdits = conn;
    {
        PatchResult r0 = patchXrickBinaryFull(argv[1], &conn);
        if (!r0.ok) { std::printf("FAIL no-op patch: %s\n", r0.message.c_str()); return 1; }
        ConnectionsData reloaded0;
        if (!loadXrickConnections(r0.outputPath, reloaded0, err)) { std::printf("FAIL reload: %s\n", err.c_str()); return 1; }
        bool ok = true;
        for (int i = 0; i < MAP_NBR_SUBMAPS && ok; i++)
        {
            if (reloaded0.exits[i].size() != beforeEdits.exits[i].size()) { ok = false; break; }
            for (size_t j = 0; j < beforeEdits.exits[i].size(); j++)
                if (!sameExit(reloaded0.exits[i][j], beforeEdits.exits[i][j])) { ok = false; break; }
        }
        std::remove(r0.outputPath.string().c_str());
        if (!ok) { std::printf("FAIL: no-op repack+patch+reload changed some submap's exits\n"); return 1; }
        std::printf("OK: no-op repack+patch+reload preserves every submap's exits exactly\n");
    }

    // Round-trip #2: this binary's connect table is already 100% full
    // (106 exits + 47 end-markers = 153/153 slots), so adding a brand-new
    // exit isn't possible without deleting one first -- exercised below.
    // Here: an ordinary, always-possible edit -- move an existing link's
    // absolute rows (still within the same submap's valid local range).
    ConnectEntry edited = conn.exits[0][0];
    edited.rowAbs += 1;
    conn.exits[0][0] = edited;

    PatchResult r = patchXrickBinaryFull(argv[1], &conn);
    std::printf("patch: ok=%d msg=%s\n", r.ok, r.message.c_str());
    if (!r.ok) return 1;

    ConnectionsData reloaded;
    if (!loadXrickConnections(r.outputPath, reloaded, err)) { std::printf("FAIL reload: %s\n", err.c_str()); return 1; }
    if (!sameExit(reloaded.exits[0][0], edited))
    {
        std::printf("FAIL: edited exit mismatch after patch+reload (got dir=%d row=%d target=%d targetrow=%d)\n",
                     reloaded.exits[0][0].dir, reloaded.exits[0][0].rowAbs,
                     reloaded.exits[0][0].targetSubmap, reloaded.exits[0][0].targetRowAbs);
        return 1;
    }
    for (int i = 1; i < MAP_NBR_SUBMAPS; i++)
    {
        if (reloaded.exits[i].size() != conn.exits[i].size()) { std::printf("FAIL: submap %d exit count changed\n", i); return 1; }
        for (size_t j = 0; j < conn.exits[i].size(); j++)
            if (!sameExit(reloaded.exits[i][j], conn.exits[i][j])) { std::printf("FAIL: submap %d exit %zu mismatch\n", i, j); return 1; }
    }
    std::remove(r.outputPath.string().c_str());
    std::printf("OK: edited link round-trips through patch+reload, all other submaps untouched\n");

    // Capacity check: refuses to overflow the 153-slot table.
    {
        ConnectionsData overflowTest = conn;
        overflowTest.exits[0].push_back(ConnectEntry{0, submapStartRow(conn.submaps[0]), 2, 7});
        std::string capErr;
        if (repackConnections(overflowTest, capErr)) { std::printf("FAIL: repack should have refused to exceed capacity\n"); return 1; }
        std::printf("OK: capacity check correctly refuses to overflow the 153-slot table (%s)\n", capErr.c_str());
    }

    // Range check: an absolute row far outside the owning submap's local
    // 0-255 window must be rejected too.
    {
        ConnectionsData rangeTest = conn;
        rangeTest.exits[0][0].rowAbs = submapStartRow(conn.submaps[0]) + 9999;
        std::string rangeErr;
        if (repackConnections(rangeTest, rangeErr)) { std::printf("FAIL: repack should have refused an out-of-range row\n"); return 1; }
        std::printf("OK: out-of-range absolute row correctly refused (%s)\n", rangeErr.c_str());
    }

    // disconnectSubmap(): clears its own exits and redirects incoming ones.
    {
        ConnectionsData delTest = conn;
        // find some submap that has at least one other submap pointing to it
        int target = -1, from = -1;
        for (int i = 0; i < MAP_NBR_SUBMAPS && target < 0; i++)
            for (auto &e : delTest.exits[i])
                if (e.targetSubmap != SUBMAP_END_OF_LEVEL) { target = e.targetSubmap; from = i; break; }
        if (target < 0) { std::printf("FAIL: could not find any submap-to-submap link to test disconnect\n"); return 1; }
        int redirected = disconnectSubmap(delTest, target);
        if (redirected < 1) { std::printf("FAIL: expected at least 1 redirected link, got %d\n", redirected); return 1; }
        if (!delTest.exits[target].empty()) { std::printf("FAIL: disconnected submap still has outgoing exits\n"); return 1; }
        bool stillPoints = false;
        for (auto &e : delTest.exits[from]) if (e.targetSubmap == target) stillPoints = true;
        if (stillPoints) { std::printf("FAIL: submap %d still points at disconnected submap %d\n", from, target); return 1; }
        std::printf("OK: disconnectSubmap() clears exits and redirects %d incoming link(s)\n", redirected);
    }

    // .map v2 round-trip: save with connections, corrupt in memory, reload, compare.
    {
        fs::path tmp = fs::temp_directory_path() / "rickeditor_test_conn.map";
        std::string ferr;
        if (!saveMapFileWithConnections(tmp, conn, ferr)) { std::printf("FAIL save .map v2: %s\n", ferr.c_str()); return 1; }

        ConnectionsData corrupted;
        corrupted.loaded = true;
        for (int i = 0; i < MAP_NBR_SUBMAPS; i++) { corrupted.submaps[i] = {0, 0, 0}; corrupted.exits[i].clear(); }

        if (!loadMapFileWithConnections(tmp, corrupted, ferr)) { std::printf("FAIL load .map v2: %s\n", ferr.c_str()); return 1; }
        bool ok = true;
        for (int i = 0; i < MAP_NBR_SUBMAPS && ok; i++)
        {
            if (corrupted.submaps[i].page != conn.submaps[i].page || corrupted.submaps[i].bnum != conn.submaps[i].bnum
             || corrupted.submaps[i].mark != conn.submaps[i].mark || corrupted.exits[i].size() != conn.exits[i].size())
            { ok = false; break; }
            for (size_t j = 0; j < conn.exits[i].size(); j++)
                if (!sameExit(corrupted.exits[i][j], conn.exits[i][j])) { ok = false; break; }
        }
        std::remove(tmp.string().c_str());
        if (!ok) { std::printf("FAIL: .map v2 round-trip mismatch\n"); return 1; }
        std::printf("OK: .map v2 (level + connections) round-trips exactly\n");
    }

    return 0;
}
