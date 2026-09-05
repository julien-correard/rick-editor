// Verifie la retrocompatibilite : un fichier RKM9 (sans le bloc bank-0
// ajoute par RKMA) doit toujours se charger sans erreur, en laissant
// tiles_data[0] tel qu'il etait avant le chargement.
#define STB_IMAGE_IMPLEMENTATION
#include "mapfile.h"
#include "xrick_marks.h"
#include "sprite_import.h"
#include <cstdio>
#include <cstring>
#include <fstream>

int main()
{
    std::string err;
    ConnectionsData conn = defaultConnections();
    MarksData marks = defaultMarks();
    EflgData eflg = defaultEflg();
    std::array<ImapText, SCREEN_IMAPTEXT_COUNT> texts = defaultImapTexts();

    fs::path tmp = fs::temp_directory_path() / "rickeditor_test_rkma_full.map";
    if (!saveMapFileWithSprites(tmp, conn, marks, eflg, texts, false, err)) { std::printf("FAIL save: %s\n", err.c_str()); return 1; }

    // Tronque le fichier pour retirer le bloc bank-0 (256 * sizeof(tile_t)
    // = 256*32 = 8192 octets a la fin) et change le magic en "RKM9".
    size_t fullSize = fs::file_size(tmp);
    size_t truncatedSize = fullSize - 256 * 32;
    fs::path truncated = fs::temp_directory_path() / "rickeditor_test_rkm9_simulated.map";
    {
        std::ifstream in(tmp, std::ios::binary);
        std::vector<char> buf(truncatedSize);
        in.read(buf.data(), (std::streamsize)truncatedSize);
        in.close();
        buf[3] = '9'; // magic RKMA -> RKM9
        std::ofstream out(truncated, std::ios::binary);
        out.write(buf.data(), (std::streamsize)buf.size());
    }

    // Marque tiles_data[0] avec une valeur sentinelle avant de charger --
    // doit rester intacte (le fichier RKM9 simule ne doit pas y toucher).
    for (int i = 0; i < 8; i++) tiles_data[0][10][i] = 0xCAFEBABE;

    ConnectionsData conn2; MarksData marks2; EflgData eflg2;
    std::array<ImapText, SCREEN_IMAPTEXT_COUNT> texts2;
    bool ok = loadMapFileWithSprites(truncated, conn2, marks2, eflg2, texts2, err);
    if (!ok) { std::printf("FAIL load (should succeed on RKM9): %s\n", err.c_str()); return 1; }

    for (int i = 0; i < 8; i++)
        if (tiles_data[0][10][i] != 0xCAFEBABE) { ok = false; std::printf("FAIL: bank0 tile touched despite RKM9 file\n"); }

    std::printf(ok ? "OK: RKM9 file (no bank-0 block) still loads fine, bank 0 left untouched\n" : "FAIL\n");
    return ok ? 0 : 1;
}
