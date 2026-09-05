// map2c.cpp -- converts an RUxF .map (RKMC) into the C data file the xrick
// RUxF engine compiles as `game_ruxf.c`.
//
// It reuses this project's own packers (repackConnections / repackMarks /
// loadMapFileWithSprites / encodeImapTextRaw) so the engine's arrays match,
// byte for byte, what the editor would write into a patched binary -- no
// re-implementation drift.
//
//   rickeditor-map2c <input.map> <output.c>
//
// The emitted file defines, with the RUxF widening (U16 blocks/map_bnums,
// 1024 blocks, 4 game-tile pages in banks 1-4, flat U8 map_eflg[0x400]):
//   tile_t     tiles_data[TILES_NBR_BANKS][0x100]
//   block_t    map_blocks[MAP_NBR_BLOCKS]      (U16[16], absolute tile idx)
//   U16        map_bnums[MAP_NBR_BNUMS]
//   U8         map_eflg[MAP_NBR_TILES]
//   submap_t   map_submaps[MAP_NBR_SUBMAPS]
//   connect_t  map_connect[MAP_NBR_CONNECT]
//   mark_t     map_marks[MAP_NBR_MARKS]
//   map_t      map_maps[MAP_NBR_MAPS]
//   sprite_t   sprites_data[SPRITES_NBR_SPRITES]
//   U8         screen_imaptext_*[] + U8 *screen_imaptext[5]
//
// Use ./tools/map2c.sh (or the CMake `map2c` target) to build it.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <array>
#include <filesystem>
#include <algorithm>

#include "config.h"
#include "tiles.h"        // tile_t, tiles_data, TILES_NBR_BANKS
#include "sprites.h"      // sprite_t, sprites_data, SPRITES_NBR_SPRITES
#include "mapdata.h"      // map_bnums[0x1FD8], map_blocks[0x400][16] (editor global copies)
#include "xrick_levels.h" // submapStartRow, ConnectionsData, repackConnections
#include "xrick_marks.h"  // MarksData, repackMarks, loadMapFileWithSprites, EflgData
#include "xrick_eflg.h"
#include "screens_text.h" // ImapText, SCREEN_IMAPTEXT_COUNT, encodeImapTextRaw

namespace fs = std::filesystem;

// ---- emit helpers -------------------------------------------------------

static void emitVal(FILE *f, unsigned long v, unsigned &col)
{
    fprintf(f, "0x%04lx, ", v);
    col++;
    if (col >= 10) { fputc('\n', f); col = 0; }
}

static void emitU32Elem(FILE *f, const U32 *rows, int n)
{
    unsigned col = 0;
    fputc('{', f);
    for (int i = 0; i < n; i++) emitVal(f, (unsigned long)rows[i], col);
    fputs("0},\n", f); // keep the common trailing 0, harmless & matches style
}

// ---- tile rows ----------------------------------------------------------

// The engine's tile_t is U32[0x08] (GFXST) or U16[0x08] (GFXPC). The editor
// builds GFXST, so tiles_data is U32 based and we emit its 8 U32 rows.
static void emitTiles(FILE *f)
{
    fprintf(f, "\ntile_t tiles_data[TILES_NBR_BANKS][0x100] = {\n");
    for (int b = 0; b < TILES_NBR_BANKS; b++)
    {
        fprintf(f, "  { /* BANK %d */\n", b);
        for (int t = 0; t < 0x100; t++)
        {
            fprintf(f, "    { ");
            for (int r = 0; r < 8; r++)
            {
                fprintf(f, "0x%08lx", (unsigned long)tiles_data[b][t][r]);
                if (r != 7) fputs(", ", f);
            }
            fputs(" },\n", f);
        }
        fprintf(f, "  },\n");
    }
    fprintf(f, "};\n");
}

// ---- blocks -------------------------------------------------------------

static void emitBlocks(FILE *f)
{
    fprintf(f, "\nblock_t map_blocks[MAP_NBR_BLOCKS] = {\n");
    for (int b = 0; b < 0x400; b++)
    {
        fprintf(f, "  {");
        for (int i = 0; i < 16; i++)
            fprintf(f, "0x%03x%s", (unsigned)map_blocks[b][i], (i == 15) ? "" : ", ");
        fprintf(f, "}%s\n", (b == 0x3ff) ? "" : ",");
    }
    fprintf(f, "};\n");
}

// ---- bnums --------------------------------------------------------------

static void emitBnums(FILE *f)
{
    fprintf(f, "\nU16 map_bnums[MAP_NBR_BNUMS] = {\n");
    for (int i = 0; i < 0x1FD8; i++)
    {
        if (i % 16 == 0) fputs("  ", f);
        fprintf(f, "0x%03x%s", (unsigned)map_bnums[i], ((i & 15) == 15 || i == 0x1FD8 - 1) ? ",\n" : ", ");
    }
    fprintf(f, "};\n");
}

// ---- eflg ---------------------------------------------------------------

static void emitEflg(FILE *f, const EflgData &eflg)
{
    fprintf(f, "\nU8 map_eflg[MAP_NBR_TILES] = {\n");
    for (int p = 0; p < 4; p++)
        for (int i = 0; i < 256; i++)
        {
            if ((p * 256 + i) % 16 == 0) fputs("  ", f);
            fprintf(f, "0x%02x%s", (unsigned)eflg.bank[p][i],
                    ((p * 256 + i) % 16 == 15 || (p == 3 && i == 255)) ? ",\n" : ", ");
        }
    fprintf(f, "};\n");
}

// ---- submaps / connect / marks / maps -----------------------------------

static void emitSubmaps(FILE *f, const ConnectionsData &conn, const MarksData &marks)
{
    fprintf(f, "\nsubmap_t map_submaps[MAP_NBR_SUBMAPS] = {\n");
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
    {
        uint8_t page_b[2], bnum_b[2], connect_b[2], mark_b[2];
        std::memcpy(&page_b,    &conn.packedSubmaps[i * 8 + 0], 2);
        std::memcpy(&bnum_b,    &conn.packedSubmaps[i * 8 + 2], 2);
        std::memcpy(&connect_b, &conn.packedSubmaps[i * 8 + 4], 2);
        std::memcpy(&mark_b, &conn.packedSubmaps[i * 8 + 6], 2);
        uint16_t page = (uint16_t)(page_b[0] | (page_b[1] << 8));
        uint16_t bnum = (uint16_t)(bnum_b[0] | (bnum_b[1] << 8));
        uint16_t connect = (uint16_t)(connect_b[0] | (connect_b[1] << 8));
        uint16_t mark = (uint16_t)(mark_b[0] | (mark_b[1] << 8));
        fprintf(f, "  {0x%04x, 0x%04x, 0x%04x, 0x%04x}%s\n",
                page, bnum, connect, mark, (i == MAP_NBR_SUBMAPS - 1) ? "" : ",");
    }
    fprintf(f, "};\n");
}

static void emitConnect(FILE *f, const ConnectionsData &conn)
{
    bool ruxf = (conn.packedConnect.size() == (size_t)MAP_NBR_CONNECT * 8); // 1224 bytes, 8-byte connect_t
    int count = MAP_NBR_CONNECT;
    int width = ruxf ? 8 : 4;
    fprintf(f, "\nconnect_t map_connect[MAP_NBR_CONNECT] = {\n");
    for (int i = 0; i < count; i++)
    {
        const uint8_t *c = &conn.packedConnect[i * width];
        if (ruxf)
            // {dir, submap, colin, pad, rowout(U16), rowin(U16)}
            fprintf(f, "  {%d, %d, %d, 0, 0x%04x, 0x%04x}%s\n",
                    c[0], c[1], c[2], (uint16_t)(c[4] | (c[5] << 8)), (uint16_t)(c[6] | (c[7] << 8)), (i == count - 1) ? "" : ",");
        else
            fprintf(f, "  {0x%02x, 0x%02x, 0x%02x, 0x%02x}%s\n",
                    c[0], c[1], c[2], c[3], (i == count - 1) ? "" : ",");
    }
    fprintf(f, "};\n");
}

static void emitMarks(FILE *f, const MarksData &marks)
{
    fprintf(f, "\nmark_t map_marks[MAP_NBR_MARKS] = {\n");
    for (int i = 0; i < MAP_NBR_MARKS; i++)
    {
        const uint8_t *m = &marks.packedMarks[i * 5];
        fprintf(f, "  {0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x}%s\n",
                m[0], m[1], m[2], m[3], m[4], (i == MAP_NBR_MARKS - 1) ? "" : ",");
    }
    fprintf(f, "};\n");
}

static const char *TUNES[] = {
    "sounds/tune0.wav", "sounds/tune1.wav", "sounds/tune2.wav",
    "sounds/tune3.wav", "sounds/tune4.wav",
};

static void emitMapMaps(FILE *f, const ConnectionsData &conn)
{
    fprintf(f, "\nmap_t map_maps[MAP_NBR_MAPS] = {\n");
    for (int i = 0; i < MAP_NBR_MAPS; i++)
    {
        int sm = conn.mapStarts[i].submap;
        int base = (sm >= 0 && sm < MAP_NBR_SUBMAPS) ? submapStartRow(conn.submaps[sm]) : 0;
        int rawRow = conn.mapStarts[i].row - base;
        rawRow = (rawRow / 4) * 4;
        if (rawRow < 0) rawRow = 0;
        if (rawRow > 255) rawRow = 255;
        fprintf(f, "  {0x%04x, 0x%04x, 0x%04x, 0x%04x, \"%s\"}%s\n",
                (unsigned)conn.mapStarts[i].x,
                (unsigned)conn.mapStarts[i].y,
                (unsigned)rawRow,
                (unsigned)(conn.mapStarts[i].submap & 0xffff),
                TUNES[i],
                (i == MAP_NBR_MAPS - 1) ? "" : ",");
    }
    fprintf(f, "};\n");
}

// ---- sprites (GFXST flat U32[0x54] matches editor sprite_t U32[0x15][4]) --

static void emitSprites(FILE *f)
{
    fprintf(f, "\nsprite_t sprites_data[SPRITES_NBR_SPRITES] = {\n");
    for (int s = 0; s < SPRITES_NBR_SPRITES; s++)
    {
        fprintf(f, "  { /* %06d */\n    ", s);
        for (int i = 0; i < 0x54; i++)
        {
            if (i && (i % 4) == 0) fputs("    ", f);
            fprintf(f, "0x%08lx, ", (unsigned long)sprites_data[s][i / 4][i % 4]);
            if ((i % 4) == 3) fputc('\n', f);
        }
        fprintf(f, "  }%s\n", (s == SPRITES_NBR_SPRITES - 1) ? "" : ",");
    }
    fprintf(f, "};\n");
}

// ---- imaptext -----------------------------------------------------------

static const char *IMAPTEXT_SYMS[] = {
    "screen_imaptext_amazon",
    "screen_imaptext_egypt",
    "screen_imaptext_castle",
    "screen_imaptext_missile",
    "screen_imaptext_muchlater",
};

static void emitImapText(FILE *f, const std::array<ImapText, SCREEN_IMAPTEXT_COUNT> &texts)
{
    for (int i = 0; i < SCREEN_IMAPTEXT_COUNT; i++)
    {
        std::vector<uint8_t> raw = encodeImapTextRaw(texts[i]);
        fprintf(f, "\nU8 %s[] = {\n", IMAPTEXT_SYMS[i]);
        for (size_t j = 0; j < raw.size(); j++)
        {
            if (j % 16 == 0) fputs("  ", f);
            fprintf(f, "0x%02x, ", raw[j]);
            if (j % 16 == 15) fputc('\n', f);
        }
        fputs("0};\n", f);
    }
    fprintf(f, "\nU8 *screen_imaptext[5] = {\n");
    for (int i = 0; i < 5; i++)
        fprintf(f, "  %s,\n", IMAPTEXT_SYMS[i]);
    fprintf(f, "};\n");
}

// ---- main ---------------------------------------------------------------

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: rickeditor-map2c <input.map> <output.c>\n");
        return 1;
    }

    fs::path in = argv[1];
    fs::path out = argv[2];

    ConnectionsData conn = defaultConnections();
    MarksData marks = defaultMarks();
    EflgData eflg;
    std::array<ImapText, SCREEN_IMAPTEXT_COUNT> texts; // filled by loadMapFileWithSprites
    std::string err;
    bool ruxp = false;

    if (!loadMapFileWithSprites(in, conn, marks, eflg, texts, err, &ruxp))
    {
        fprintf(stderr, "load %s failed: %s\n", in.string().c_str(), err.c_str());
        return 1;
    }
    if (!ruxp)
    {
        fprintf(stderr, "%s is not an RUxF (RKMC) map.\n", in.string().c_str());
        return 1;
    }

    if (!repackConnections(conn, err, ruxp))
    {
        fprintf(stderr, "repackConnections failed: %s\n", err.c_str());
        return 1;
    }
    if (!repackMarks(marks, conn, err))
    {
        fprintf(stderr, "repackMarks failed: %s\n", err.c_str());
        return 1;
    }
    // repackConnections() above copies each submap's `mark` pointer from the
    // loaded .map verbatim, but repackMarks() rebuilds the linear mark array
    // in a DIFFERENT layout (marks sorted by row + one 0xff end-marker per
    // submap). The engine scans a submap's marks starting at
    // map_submaps[].mark and reads forward to the 0xff terminator, so the
    // stored pointers MUST equal marks.markStart[], or the engine starts at
    // the wrong slot and silently drops/skips entities. Sync them now.
    for (int i = 0; i < MAP_NBR_SUBMAPS; i++)
    {
        uint16_t mark = marks.markStart[i];
        std::memcpy(&conn.packedSubmaps[i * 8 + 6], &mark, 2);
        conn.submaps[i].mark = (int)mark;
    }

    {
        // Sanity-guard the loaded RUxF data before emitting. A block cell is
        // an ABSOLUTE tile index (0-1023), so any out-of-range value means the
        // .map's block section is corrupt (e.g. a failed/legacy load filled
        // map_blocks with sprite/tile graphics). Fail fast instead of emitting
        // error-free but broken game data.
        int badBlocks = 0;
        for (int b = 0; b < 0x400; b++)
            for (int i = 0; i < 16; i++)
                if ((unsigned)map_blocks[b][i] >= 0x400)
                    badBlocks++;
        if (badBlocks)
        {
            std::fprintf(stderr, "%s is corrupt: %d out-of-range block cell(s) (block cells must be absolute tile indices 0-1023).\n",
                         in.string().c_str(), badBlocks);
            return 1;
        }
    }

    FILE *f = std::fopen(out.string().c_str(), "wb");
    if (!f)
    {
        fprintf(stderr, "cannot write %s\n", out.string().c_str());
        return 1;
    }

    fprintf(f, "/* Generated by rickeditor-map2c from %s -- RUxF (RKMC) data.\n", in.string().c_str());
    fprintf(f, "   DO NOT EDIT: regenerated from the editor's .map format. */\n");
    fprintf(f, "#include \"system.h\"\n");
    fprintf(f, "#include \"tiles.h\"\n");
    fprintf(f, "#include \"maps.h\"\n");
    fprintf(f, "#include \"sprites.h\"\n");

    emitTiles(f);
    emitBlocks(f);
    emitBnums(f);
    emitEflg(f, eflg);
    emitSubmaps(f, conn, marks);
    emitConnect(f, conn);
    emitMarks(f, marks);
    emitMapMaps(f, conn);
    emitSprites(f);
    emitImapText(f, texts);

    std::fclose(f);
    return 0;
}