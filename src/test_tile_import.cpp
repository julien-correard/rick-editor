// test_tile_import.cpp -- verifie importTileFromImage() independamment
// de l'UI : ecrit un petit BMP 8x8 dont chaque pixel est une couleur
// EXACTE de la palette (pas d'incertitude due au moyennage/resampling),
// l'importe dans une tuile, puis relit les pixels via decode_tile() (le
// meme decodeur que l'affichage) pour verifier l'aller-retour bit a bit.
//
// Compilation manuelle (pas dans le CMake) :
//   g++ -std=c++17 -Iinclude -Isrc -Ithird_party src/test_tile_import.cpp \
//       src/dat_tilesST.c -o test_tile_import && ./test_tile_import
#define STB_IMAGE_IMPLEMENTATION
#include "tile_import.h"
#include "tiles_render.h" // decode_tile()

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

// Ecrit un BMP 24 bits non compresse de 8x8 pixels. `idx[y*8+x]` donne
// l'indice de palette (0-15) voulu pour chaque pixel ; les lignes BMP
// sont stockees bas-vers-haut, d'ou le flip.
static void writeTestBmp(const std::string &path, const uint8_t idx[64])
{
    const int W = 8, H = 8;
    int rowSize = ((W * 3 + 3) / 4) * 4; // BMP pad chaque ligne sur 4 octets
    int pixelDataSize = rowSize * H;
    int fileSize = 54 + pixelDataSize;

    std::vector<uint8_t> buf(fileSize, 0);
    // en-tete fichier BMP
    buf[0] = 'B'; buf[1] = 'M';
    *(uint32_t*)&buf[2] = fileSize;
    *(uint32_t*)&buf[10] = 54; // offset donnees pixel
    // en-tete DIB (BITMAPINFOHEADER)
    *(uint32_t*)&buf[14] = 40;
    *(int32_t*)&buf[18] = W;
    *(int32_t*)&buf[22] = H;
    *(uint16_t*)&buf[26] = 1;  // plans
    *(uint16_t*)&buf[28] = 24; // bits/pixel
    *(uint32_t*)&buf[34] = pixelDataSize;

    for (int y = 0; y < H; y++)
    {
        int srcY = H - 1 - y; // BMP : bas vers haut
        for (int x = 0; x < W; x++)
        {
            uint8_t c = idx[srcY * W + x];
            uint8_t *p = &buf[54 + y * rowSize + x * 3];
            p[0] = (uint8_t)BLUE[c];  // BMP stocke B,G,R
            p[1] = (uint8_t)GREEN[c];
            p[2] = (uint8_t)RED[c];
        }
    }

    FILE *f = std::fopen(path.c_str(), "wb");
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
}

int main()
{
    // Motif de test : les 16 couleurs de la palette, chacune deux fois,
    // dans un ordre qui n'est ni trie ni symetrique (pour detecter une
    // eventuelle inversion ligne/colonne dans l'encodage).
    uint8_t idx[64];
    for (int i = 0; i < 64; i++) idx[i] = (uint8_t)((i * 7 + 3) % 16);

    fs::path bmpPath = fs::temp_directory_path() / "rickeditor_test_tile.bmp";
    writeTestBmp(bmpPath.string(), idx);

    tile_t tile{};
    std::string err;
    if (!importTileFromImage(bmpPath, tile, err))
    {
        std::printf("FAIL import: %s\n", err.c_str());
        return 1;
    }

    Uint32 out[64];
    // decode_tile() lit tiles_data[bank][NbTile] -- on ecrit donc notre
    // tuile de test directement dedans (bank/emplacement arbitraires,
    // ici bank 1 tuile 5) avant de decoder, plutot que de dupliquer
    // l'algorithme de decode_tile().
    std::memcpy(tiles_data[1][5], tile, sizeof(tile_t));
    decode_tile(1, 5, out);

    int fails = 0;
    for (int i = 0; i < 64; i++)
    {
        uint8_t c = idx[i];
        uint32_t expected = (uint32_t)RED[c] | ((uint32_t)GREEN[c] << 8) | ((uint32_t)BLUE[c] << 16) | (0xFFu << 24);
        if (out[i] != expected)
        {
            std::printf("MISMATCH pixel %d (row %d col %d): got 0x%08x, expected 0x%08x (idx %d)\n",
                        i, i / 8, i % 8, out[i], expected, c);
            fails++;
        }
    }

    if (fails)
    {
        std::printf("FAIL: %d/64 pixels mismatched\n", fails);
        return 1;
    }

    std::printf("OK: 64/64 pixels round-tripped exactly through importTileFromImage()+decode_tile()\n");
    return 0;
}
