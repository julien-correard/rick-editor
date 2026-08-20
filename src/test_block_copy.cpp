// (voir aussi EDITEUR.md, section "Block Editor")
// Test de la logique de copie d'un block vers un autre (memcpy sur
// map_blocks[dst] <- map_blocks[src]) : verifie que la destination
// recoit exactement le contenu de la source, que la source reste
// inchangee (contrairement au swap), et qu'aucun autre block n'est
// affecte.
#include "mapdata.h"
#include <cstdio>
#include <cstring>

int main()
{
    int refOther[16];
    std::memcpy(refOther, map_blocks[99], sizeof(refOther));

    for (int i = 0; i < 16; i++) { map_blocks[10][i] = 50 + i; map_blocks[20][i] = 150 - i; }
    int srcRef[16];
    std::memcpy(srcRef, map_blocks[10], sizeof(srcRef));

    std::memcpy(map_blocks[20], map_blocks[10], sizeof(map_blocks[20]));

    bool ok = true;
    if (std::memcmp(map_blocks[20], srcRef, sizeof(srcRef)) != 0) { ok = false; std::printf("FAIL: block 20 doesn't have block 10's content after copy\n"); }
    if (std::memcmp(map_blocks[10], srcRef, sizeof(srcRef)) != 0) { ok = false; std::printf("FAIL: source block 10 changed -- copy should leave it untouched\n"); }
    if (std::memcmp(map_blocks[99], refOther, sizeof(refOther)) != 0) { ok = false; std::printf("FAIL: unrelated block 99 was affected\n"); }

    std::printf(ok ? "OK: block copy exact, source untouched, no side effects\n" : "FAIL\n");
    return ok ? 0 : 1;
}
