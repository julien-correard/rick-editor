// Test de la detection des cases "vides" en batch import (sprites) :
// grille 3x1 sprites 32x21, une case uniforme opaque (ignoree), une
// case avec RGB bruite mais entierement transparente (ignoree aussi,
// c'est le cas que isCellFullyTransparent() est cense couvrir en plus
// de isCellUniformRGBA()), une case normale (importee).
#define STB_IMAGE_IMPLEMENTATION
#include "sprite_import.h"
#include <cstdio>
#include <cstring>

static void writeTestTga(const std::string &path, int W, int H, const std::vector<std::array<int,4>> &px /* RGBA par pixel, -1 pour indice palette sinon RGBA direct */)
{
    std::vector<uint8_t> buf(18 + (size_t)W*H*4, 0);
    buf[2] = 2;
    buf[12] = W & 0xFF; buf[13] = (W>>8)&0xFF;
    buf[14] = H & 0xFF; buf[15] = (H>>8)&0xFF;
    buf[16] = 32;
    buf[17] = 0x28;
    for (int i = 0; i < W*H; i++)
    {
        auto &c = px[i];
        uint8_t *p = &buf[18 + (size_t)i*4];
        p[0]=(uint8_t)c[2]; p[1]=(uint8_t)c[1]; p[2]=(uint8_t)c[0]; p[3]=(uint8_t)c[3]; // BGRA
    }
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(buf.data(), 1, buf.size(), f);
    fclose(f);
}

int main()
{
    int W = SPRITE_W * 3, H = SPRITE_H;
    std::vector<std::array<int,4>> px(W*H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            int cell = x / SPRITE_W;
            int lx = x % SPRITE_W;
            if (cell == 0)
            {
                // case normale : motif opaque varie
                int c = ((lx+y)%2) ? 200 : 50;
                px[y*W+x] = {c, c, c, 255};
            }
            else if (cell == 1)
            {
                // case uniforme opaque (couleur fixe partout) -> vide
                px[y*W+x] = {120, 80, 40, 255};
            }
            else
            {
                // case "bruit RGB sous transparence" -> entierement
                // transparente (alpha=0) mais RGB different a chaque pixel
                px[y*W+x] = {(x*37+y*11)%256, (x*13)%256, (y*29)%256, 0};
            }
        }

    fs::path tgaPath = fs::temp_directory_path() / "rickeditor_test_sprite_batch_empty.tga";
    writeTestTga(tgaPath.string(), W, H, px);

    int startSprite = 60;
    for (int i = 0; i < 3; i++)
        for (int r = 0; r < SPRITE_H; r++)
            for (int c4 = 0; c4 < 4; c4++)
                sprites_data[startSprite+i][r][c4] = 0xCAFEF00D;

    SpriteBatchImportResult res;
    std::string err;
    if (!importSpritesBatchFromImage(tgaPath, startSprite, res, err))
    { printf("FAIL import: %s\n", err.c_str()); return 1; }

    printf("cols=%d rows=%d imported=%d skippedUniform=%d\n", res.cols, res.rows, res.imported, res.skippedUniform);

    bool ok = true;
    if (res.imported != 1) { ok = false; printf("FAIL: expected imported=1\n"); }
    if (res.skippedUniform != 2) { ok = false; printf("FAIL: expected skippedUniform=2\n"); }

    // Cases 1 (uniforme) et 2 (transparente bruitee) doivent garder la sentinelle.
    for (int p : {1, 2})
    {
        bool stillSentinel = true;
        for (int r = 0; r < SPRITE_H && stillSentinel; r++)
            for (int c4 = 0; c4 < 4; c4++)
                if (sprites_data[startSprite+p][r][c4] != 0xCAFEF00D) stillSentinel = false;
        if (!stillSentinel) { ok = false; printf("FAIL: empty/transparent cell %d was overwritten!\n", p); }
    }
    // Case 0 doit avoir change.
    {
        bool stillSentinel = true;
        for (int r = 0; r < SPRITE_H && stillSentinel; r++)
            for (int c4 = 0; c4 < 4; c4++)
                if (sprites_data[startSprite][r][c4] != 0xCAFEF00D) stillSentinel = false;
        if (stillSentinel) { ok = false; printf("FAIL: non-empty cell 0 was NOT imported!\n"); }
    }

    printf(ok ? "OK: sprite uniform/transparent-cell skip logic exact\n" : "FAIL\n");
    return ok ? 0 : 1;
}
