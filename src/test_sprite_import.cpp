// Test de importSpriteFromImage() et importSpritesBatchFromImage() :
// verifie le round-trip exact (import -> decode_sprite()) pour un
// sprite unique (32x21 pile, aucun rééchantillonnage attendu) et pour
// un import par lot (grille 2x1 de sprites 32x21, avec une couleur
// distincte par sprite pour detecter tout melange/decalage), plus la
// gestion de la transparence (alpha bas -> indice 0) et le depassement
// en fin de table. Utilise le format TGA (32bpp non compresse, alpha
// natif) pour un import sans ambiguite -- plus simple a generer
// correctement qu'un BMP 32bpp avec masques BI_BITFIELDS.
#define STB_IMAGE_IMPLEMENTATION
#include "sprite_import.h"
#include <cstdio>
#include <cstring>

// idxOrNeg1[y*W+x] : indice de palette (0-15), ou -1 pour transparent.
static void writeTestTga(const std::string &path, int W, int H, const int *idxOrNeg1)
{
    std::vector<uint8_t> buf(18 + (size_t)W * H * 4, 0);
    buf[2] = 2; // uncompressed true-color
    buf[12] = W & 0xFF; buf[13] = (W >> 8) & 0xFF;
    buf[14] = H & 0xFF; buf[15] = (H >> 8) & 0xFF;
    buf[16] = 32; // bpp
    buf[17] = 0x28; // 8 bits alpha + top-left origin (rows stored top-to-bottom)

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            int v = idxOrNeg1[y * W + x];
            uint8_t *p = &buf[18 + (size_t)(y * W + x) * 4];
            if (v < 0) { p[0]=0; p[1]=0; p[2]=0; p[3]=0; } // B,G,R,A -- transparent
            else { p[0]=(uint8_t)BLUE[v]; p[1]=(uint8_t)GREEN[v]; p[2]=(uint8_t)RED[v]; p[3]=255; }
        }
    FILE *f = std::fopen(path.c_str(), "wb");
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
}

int main()
{
    // --- Test 1 : sprite unique 32x21 pile, avec transparence ---
    std::vector<int> single(SPRITE_W * SPRITE_H, 5); // fond = couleur 5
    single[0] = -1; // pixel (0,0) transparent
    single[SPRITE_W - 1] = 9; // pixel (31,0) = couleur 9 (marqueur coin)

    fs::path p1 = fs::temp_directory_path() / "rickeditor_test_sprite_single.tga";
    writeTestTga(p1.string(), SPRITE_W, SPRITE_H, single.data());

    sprite_t spr{};
    std::string err;
    if (!importSpriteFromImage(p1, spr, err)) { std::printf("FAIL single import: %s\n", err.c_str()); return 1; }
    std::memcpy(sprites_data[7], spr, sizeof(sprite_t));

    Uint32 out[SPRITE_W * SPRITE_H];
    decode_sprite(7, out);
    bool ok = true;
    auto colOf = [](int i){ return (uint32_t)RED[i] | ((uint32_t)GREEN[i]<<8) | ((uint32_t)BLUE[i]<<16) | 0xFF000000u; };
    for (int i = 0; i < SPRITE_W * SPRITE_H; i++)
    {
        uint32_t expected = (single[i] < 0) ? 0u : colOf(single[i]);
        if (out[i] != expected) { ok = false; std::printf("MISMATCH single px %d: got 0x%08x exp 0x%08x\n", i, out[i], expected); }
    }
    std::printf(ok ? "OK: single sprite import round-trips exactly (incl. transparency)\n" : "FAIL single\n");

    // --- Test 2 : import par lot, grille 2x1 de sprites (64x21), avec
    // marqueurs distincts pour verifier l'absence de melange ---
    int W2 = SPRITE_W * 2, H2 = SPRITE_H;
    std::vector<int> grid(W2 * H2, -1);
    for (int y = 0; y < H2; y++)
    for (int x = 0; x < W2; x++)
    {
        int col = x / SPRITE_W; // 0 ou 1
        grid[y * W2 + x] = (col == 0) ? 3 : 12;
    }
    fs::path p2 = fs::temp_directory_path() / "rickeditor_test_sprite_batch.tga";
    writeTestTga(p2.string(), W2, H2, grid.data());

    SpriteBatchImportResult br;
    if (!importSpritesBatchFromImage(p2, 200, br, err)) { std::printf("FAIL batch import: %s\n", err.c_str()); return 1; }
    std::printf("batch: cols=%d rows=%d imported=%d skipped=%d endSprite=%d\n", br.cols, br.rows, br.imported, br.skippedOverflow, br.endSprite);
    if (br.cols != 2 || br.rows != 1 || br.imported != 2 || br.skippedOverflow != 0)
    { ok = false; std::printf("FAIL: unexpected batch geometry/counts\n"); }

    Uint32 outA[SPRITE_W * SPRITE_H], outB[SPRITE_W * SPRITE_H];
    decode_sprite(200, outA);
    decode_sprite(201, outB);
    for (int i = 0; i < SPRITE_W * SPRITE_H; i++)
    {
        if (outA[i] != colOf(3)) { ok = false; std::printf("MISMATCH batch sprite200 px %d: got 0x%08x\n", i, outA[i]); }
        if (outB[i] != colOf(12)) { ok = false; std::printf("MISMATCH batch sprite201 px %d: got 0x%08x\n", i, outB[i]); }
    }

    // --- Test 3 : depassement en fin de table (SPRITES_NBR_SPRITES-1 = 212) ---
    SpriteBatchImportResult br2;
    if (!importSpritesBatchFromImage(p2, SPRITES_NBR_SPRITES - 1, br2, err)) { std::printf("FAIL overflow test: %s\n", err.c_str()); return 1; }
    if (br2.imported != 1 || br2.skippedOverflow != 1 || br2.endSprite != SPRITES_NBR_SPRITES - 1)
    { ok = false; std::printf("FAIL overflow: imported=%d skipped=%d endSprite=%d\n", br2.imported, br2.skippedOverflow, br2.endSprite); }

    std::printf(ok ? "OK: all sprite import tests passed\n" : "FAIL\n");
    return ok ? 0 : 1;
}
