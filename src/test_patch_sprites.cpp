// Test isole du patch ELF de sprites_data : verifie
// elf32_patch_symbol("sprites_data", ...) + loadSpritesFromXrickBinary()
// en aller-retour sur un vrai fichier ELF32.
#define STB_IMAGE_IMPLEMENTATION
#include "sprite_import.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 2) { std::printf("usage: %s <fake_xrick_elf32>\n", argv[0]); return 2; }
    fs::path target = argv[1];

    sprite_t untouchedRef;
    std::memcpy(untouchedRef, sprites_data[3], sizeof(sprite_t)); // jamais modifie ci-dessous

    for (int r = 0; r < 21; r++) for (int c = 0; c < 4; c++) sprites_data[42][r][c] = 0xDEAD0000 + r;
    for (int r = 0; r < 21; r++) for (int c = 0; c < 4; c++) sprites_data[199][r][c] = 0xC0FFEE00 + r;

    std::ifstream in(target, std::ios::binary);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    std::string err;
    if (!elf32_patch_symbol(buf, "sprites_data", sprites_data, sizeof(sprites_data), err))
    { std::printf("FAIL patch: %s\n", err.c_str()); return 1; }

    fs::path patched = fs::temp_directory_path() / "fake_xrick_sprites_patched";
    std::ofstream out(patched, std::ios::binary | std::ios::trunc);
    out.write((const char*)buf.data(), (std::streamsize)buf.size());
    out.close();

    std::memset(sprites_data, 0, sizeof(sprites_data));

    if (!loadSpritesFromXrickBinary(patched, err)) { std::printf("FAIL reload: %s\n", err.c_str()); return 1; }

    bool ok = true;
    for (int r = 0; r < 21; r++) for (int c = 0; c < 4; c++)
        if (sprites_data[42][r][c] != (uint32_t)(0xDEAD0000 + r)) { ok = false; std::printf("mismatch sprite42 row %d col %d\n", r, c); }
    for (int r = 0; r < 21; r++) for (int c = 0; c < 4; c++)
        if (sprites_data[199][r][c] != (uint32_t)(0xC0FFEE00 + r)) { ok = false; std::printf("mismatch sprite199 row %d col %d\n", r, c); }
    if (std::memcmp(sprites_data[3], untouchedRef, sizeof(sprite_t)) != 0)
    { ok = false; std::printf("untouched sprite 3 changed unexpectedly\n"); }

    std::printf(ok ? "OK: sprites_data ELF patch + reload round-trip exact\n" : "FAIL\n");
    return ok ? 0 : 1;
}
