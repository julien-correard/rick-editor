// Test round-trip du format .map RKM8 (sprites_data) : sauvegarde,
// modification en memoire, rechargement, verification bit a bit --
// plus verification qu'un sprite non modifie reste intact.
#define STB_IMAGE_IMPLEMENTATION
#include "mapfile.h"
#include "xrick_marks.h"
#include "sprite_import.h"
#include <cstdio>
#include <cstring>

int main()
{
    std::string err;
    ConnectionsData conn = defaultConnections();
    MarksData marks = defaultMarks();
    EflgData eflg = defaultEflg();

    sprite_t refSprite;
    std::memcpy(refSprite, sprites_data[50], sizeof(sprite_t));

    for (int r = 0; r < 21; r++) for (int c = 0; c < 4; c++) sprites_data[10][r][c] = 0x11223344 + r;
    for (int r = 0; r < 21; r++) for (int c = 0; c < 4; c++) sprites_data[200][r][c] = 0xAABBCC00 + r;

    fs::path tmp = fs::temp_directory_path() / "rickeditor_test_rkm8.map";
    if (!saveMapFileWithSprites(tmp, conn, marks, eflg, err)) { std::printf("FAIL save: %s\n", err.c_str()); return 1; }

    std::memset(sprites_data[10], 0, sizeof(sprite_t));
    std::memset(sprites_data[200], 0, sizeof(sprite_t));

    ConnectionsData conn2; MarksData marks2; EflgData eflg2;
    if (!loadMapFileWithSprites(tmp, conn2, marks2, eflg2, err)) { std::printf("FAIL load: %s\n", err.c_str()); return 1; }

    bool ok = true;
    for (int r = 0; r < 21; r++) for (int c = 0; c < 4; c++)
        if (sprites_data[10][r][c] != (uint32_t)(0x11223344 + r)) { ok = false; std::printf("sprite10 row %d col %d mismatch\n", r, c); }
    for (int r = 0; r < 21; r++) for (int c = 0; c < 4; c++)
        if (sprites_data[200][r][c] != (uint32_t)(0xAABBCC00 + r)) { ok = false; std::printf("sprite200 row %d col %d mismatch\n", r, c); }
    if (std::memcmp(sprites_data[50], refSprite, sizeof(sprite_t)) != 0) { ok = false; std::printf("untouched sprite 50 changed\n"); }

    std::printf(ok ? "OK: RKM8 round-trip (sprites_data) exact\n" : "FAIL: mismatch(es) above\n");
    return ok ? 0 : 1;
}
