// test_mapfile.cpp -- verifie le format .map independamment de l'UI :
// sauvegarde, modification en memoire, rechargement, egalite bit a bit.
#include <cstdio>
#include <cstdlib>
#include "mapfile.h"

int main()
{
    // 1) valeurs de reference (donnees d'origine) sauvegardees.
    std::string err;
    int reference[MAP_COUNT];
    std::memcpy(reference, map_bnums, sizeof(reference));

    fs::path tmp = fs::temp_directory_path() / "rickeditor_test.map";
    if (!saveMapFile(tmp, err)) { std::printf("FAIL save: %s\n", err.c_str()); return 1; }

    // 2) on modifie la carte en memoire pour s'assurer que le rechargement l'ecrase bien.
    map_bnums[0] = 123;
    map_bnums[MAP_COUNT - 1] = 45;

    if (!loadMapFile(tmp, err)) { std::printf("FAIL load: %s\n", err.c_str()); return 1; }

    if (std::memcmp(reference, map_bnums, sizeof(reference)) != 0)
    {
        std::printf("FAIL: round-trip mismatch\n");
        return 1;
    }

    // 3) fichier invalide correctement rejete.
    fs::path bogus = fs::temp_directory_path() / "rickeditor_bogus.map";
    { FILE* f = std::fopen(bogus.string().c_str(), "wb"); std::fputs("not a map", f); std::fclose(f); }
    if (loadMapFile(bogus, err)) { std::printf("FAIL: bogus file should have been rejected\n"); return 1; }

    std::remove(tmp.string().c_str());
    std::remove(bogus.string().c_str());
    std::printf("OK: save/load round-trip and invalid-file rejection both pass\n");
    return 0;
}
