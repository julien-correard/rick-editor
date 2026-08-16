// (voir aussi src/test_tile_import.cpp pour l'import d'une seule tuile avec rééchantillonnage)
// Test de importTilesBatchFromImage() : construit une image 20x17 (2
// tuiles pleines en largeur + 4px de reste, 2 tuiles pleines en hauteur
// + 1px de reste), chaque tuile source ayant une couleur de fond
// distincte de la palette avec UN pixel d'une autre couleur a un coin
// precis -- pour detecter tout decalage ligne/colonne ou tout melange
// entre tuiles adjacentes (contrairement a importTileFromImage, ici
// AUCUN rééchantillonnage n'est cense se produire).
#define STB_IMAGE_IMPLEMENTATION
#include "tile_import.h"
#include "tiles_render.h"
#include <cstdio>
#include <cstring>

static void writeTestBmp(const std::string &path, int W, int H, const uint8_t *idx /* W*H */)
{
    int rowSize = ((W * 3 + 3) / 4) * 4;
    int pixelDataSize = rowSize * H;
    int fileSize = 54 + pixelDataSize;
    std::vector<uint8_t> buf(fileSize, 0);
    buf[0] = 'B'; buf[1] = 'M';
    *(uint32_t*)&buf[2] = fileSize;
    *(uint32_t*)&buf[10] = 54;
    *(uint32_t*)&buf[14] = 40;
    *(int32_t*)&buf[18] = W;
    *(int32_t*)&buf[22] = H;
    *(uint16_t*)&buf[26] = 1;
    *(uint16_t*)&buf[28] = 24;
    *(uint32_t*)&buf[34] = pixelDataSize;
    for (int y = 0; y < H; y++)
    {
        int srcY = H - 1 - y;
        for (int x = 0; x < W; x++)
        {
            uint8_t c = idx[srcY * W + x];
            uint8_t *p = &buf[54 + y * rowSize + x * 3];
            p[0] = (uint8_t)BLUE[c]; p[1] = (uint8_t)GREEN[c]; p[2] = (uint8_t)RED[c];
        }
    }
    FILE *f = std::fopen(path.c_str(), "wb");
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
}

int main()
{
    const int W = 20, H = 17; // 2 cols x 2 rows pleines de tuiles + reste (4px, 1px)
    std::vector<uint8_t> idx(W * H, 0);
    // Tuile (col,row) 0..1 x 0..1 : fond = couleur (col + row*2 + 1) % 16,
    // avec un marqueur de couleur 15 au pixel (x=7,y=0) local (coin haut-droit)
    // pour verifier qu'aucune colonne/ligne n'est decalee ni melangee.
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 2; c++)
        {
            uint8_t bg = (uint8_t)((c + r * 2 + 1) % 16);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    idx[(r * 8 + y) * W + (c * 8 + x)] = bg;
            idx[(r * 8 + 0) * W + (c * 8 + 7)] = 15; // marqueur coin haut-droit local
        }
    // Reste (colonnes 16-19, lignes 16) : peu importe, jamais lu.

    fs::path bmpPath = fs::temp_directory_path() / "rickeditor_test_batch.bmp";
    writeTestBmp(bmpPath.string(), W, H, idx.data());

    BatchImportResult res;
    std::string err;
    int startTile = 250; // proche de la limite, pour tester aussi le clamp overflow
    if (!importTilesBatchFromImage(bmpPath, 1, startTile, res, err))
    { std::printf("FAIL import: %s\n", err.c_str()); return 1; }

    std::printf("cols=%d rows=%d leftoverX=%d leftoverY=%d imported=%d skipped=%d endTile=%d\n",
                res.cols, res.rows, res.leftoverPixelsX, res.leftoverPixelsY, res.imported, res.skippedOverflow, res.endTile);

    bool ok = true;
    if (res.cols != 2 || res.rows != 2) { ok = false; std::printf("FAIL: expected 2x2 grid\n"); }
    if (res.leftoverPixelsX != 4 || res.leftoverPixelsY != 1) { ok = false; std::printf("FAIL: expected leftover 4x1\n"); }
    // 4 cellules, a partir de la tuile 250 -> seules 250,251,252,253 tiennent (256-250=6 dispo, mais 4 cellules total)
    if (res.imported != 4 || res.skippedOverflow != 0) { ok = false; std::printf("FAIL: expected imported=4 skipped=0\n"); }

    // Verifie chaque tuile via decode_tile() (ordre gauche->droite, haut->bas: (0,0)->250,(1,0)->251,(0,1)->252,(1,1)->253)
    auto colOf = [](int i){ return (uint32_t)RED[i] | ((uint32_t)GREEN[i]<<8) | ((uint32_t)BLUE[i]<<16) | 0xFF000000u; };
    int expectTile[2][2] = {{250,251},{252,253}};
    for (int r = 0; r < 2; r++)
    for (int c = 0; c < 2; c++)
    {
        int t = expectTile[r][c];
        uint8_t bg = (uint8_t)((c + r * 2 + 1) % 16);
        Uint32 out[64];
        decode_tile(1, t, out);
        for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
        {
            uint8_t expIdx = (x == 7 && y == 0) ? 15 : bg;
            Uint32 exp = colOf(expIdx);
            Uint32 got = out[y*8+x];
            if (got != exp)
            {
                ok = false;
                std::printf("MISMATCH tile %d (r=%d c=%d) pixel x=%d y=%d: got 0x%08x expected 0x%08x\n", t, r, c, x, y, got, exp);
            }
        }
    }

    // Test overflow : start=254 avec 4 cellules -> seules 254,255 rentrent, 2 skipped
    BatchImportResult res2;
    if (!importTilesBatchFromImage(bmpPath, 1, 254, res2, err)) { std::printf("FAIL import2: %s\n", err.c_str()); return 1; }
    if (res2.imported != 2 || res2.skippedOverflow != 2 || res2.endTile != 255)
    { ok = false; std::printf("FAIL overflow case: imported=%d skipped=%d endTile=%d\n", res2.imported, res2.skippedOverflow, res2.endTile); }

    std::printf(ok ? "OK: batch import exact, no bleed between tiles, overflow handled\n" : "FAIL\n");
    return ok ? 0 : 1;
}
