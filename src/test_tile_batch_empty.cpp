// Test de la detection des cases "vides" en batch import (tuiles) :
// construit une image 3x2 tuiles (18x16) ou une case sur 6 est vide
// (tous pixels identiques) et une autre a une couleur uniforme
// differente (aussi vide) -- verifie que ces cases sont ignorees (ne
// modifient pas tiles_data) alors que les autres sont bien importees,
// et que le compte skippedUniform est exact.
#define STB_IMAGE_IMPLEMENTATION
#include "tile_import.h"
#include "tiles_render.h"
#include <cstdio>
#include <cstring>

static void writeTestBmp(const std::string &path, int W, int H, const uint8_t *idx /* W*H, palette index par pixel */)
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
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(buf.data(), 1, buf.size(), f);
    fclose(f);
}

int main()
{
    const int W = 24, H = 16; // grille 3x2 de tuiles 8x8
    std::vector<uint8_t> idx(W*H, 0);
    // Case (0,0): motif varie (marqueur 5, non-vide) -> doit etre importee
    for (int y=0;y<8;y++) for (int x=0;x<8;x++) idx[y*W+x] = (x==y) ? 9 : 5;
    // Case (1,0): uniforme couleur 3 -> doit etre ignoree
    for (int y=0;y<8;y++) for (int x=0;x<8;x++) idx[y*W+8+x] = 3;
    // Case (2,0): motif varie -> importee
    for (int y=0;y<8;y++) for (int x=0;x<8;x++) idx[y*W+16+x] = (x+y)%2 ? 2 : 11;
    // Case (0,1): uniforme couleur 0 (noir) -> ignoree
    for (int y=0;y<8;y++) for (int x=0;x<8;x++) idx[(8+y)*W+x] = 0;
    // Case (1,1): motif varie -> importee
    for (int y=0;y<8;y++) for (int x=0;x<8;x++) idx[(8+y)*W+8+x] = (y<4) ? 7 : 12;
    // Case (2,1): uniforme couleur 14 -> ignoree
    for (int y=0;y<8;y++) for (int x=0;x<8;x++) idx[(8+y)*W+16+x] = 14;

    fs::path bmpPath = fs::temp_directory_path() / "rickeditor_test_batch_empty.bmp";
    writeTestBmp(bmpPath.string(), W, H, idx.data());

    // Marque les 6 tuiles cibles avec une valeur sentinelle avant import,
    // pour verifier que les cases "vides" ne les touchent pas.
    int startTile = 50;
    for (int i = 0; i < 6; i++)
        for (int r = 0; r < 8; r++)
            tiles_data[1][startTile+i][r] = 0xDEADBEEF;

    BatchImportResult res;
    std::string err;
    if (!importTilesBatchFromImage(bmpPath, 1, startTile, res, err))
    { printf("FAIL import: %s\n", err.c_str()); return 1; }

    printf("cols=%d rows=%d imported=%d skippedUniform=%d skippedOverflow=%d endTile=%d\n",
           res.cols, res.rows, res.imported, res.skippedUniform, res.skippedOverflow, res.endTile);

    bool ok = true;
    if (res.imported != 3) { ok = false; printf("FAIL: expected imported=3\n"); }
    if (res.skippedUniform != 3) { ok = false; printf("FAIL: expected skippedUniform=3\n"); }
    if (res.endTile != startTile + 5) { ok = false; printf("FAIL: endTile mismatch\n"); }

    // Les 3 cases "vides" (indices 1, 3, 5 dans l'ordre de lecture : (1,0),(0,1),(2,1))
    // doivent toujours contenir la sentinelle (non touchees).
    int emptyPositions[3] = {1, 3, 5};
    for (int p : emptyPositions)
        for (int r = 0; r < 8; r++)
            if (tiles_data[1][startTile+p][r] != 0xDEADBEEF)
            { ok = false; printf("FAIL: empty cell %d was overwritten!\n", p); }

    // Les 3 cases non-vides (0, 2, 4) doivent avoir change (plus la sentinelle).
    int filledPositions[3] = {0, 2, 4};
    for (int p : filledPositions)
    {
        bool stillSentinel = true;
        for (int r = 0; r < 8; r++) if (tiles_data[1][startTile+p][r] != 0xDEADBEEF) stillSentinel = false;
        if (stillSentinel) { ok = false; printf("FAIL: non-empty cell %d was NOT imported!\n", p); }
    }

    printf(ok ? "OK: uniform-cell skip logic exact (imported/skipped correctly, no cross-contamination)\n" : "FAIL\n");
    return ok ? 0 : 1;
}
