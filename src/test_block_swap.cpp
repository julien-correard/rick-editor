// (voir aussi EDITEUR.md, section "Block Editor")
// Test de la logique de swap de deux blocks (std::swap sur
// map_blocks[a] <-> map_blocks[b], des int[16]) : verifie que le
// contenu est bien echange et qu'aucun autre block n'est affecte.
#include "mapdata.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

int main()
{
    int refOther[16];
    std::memcpy(refOther, map_blocks[99], sizeof(refOther));

    int blockA[16], blockB[16];
    for (int i = 0; i < 16; i++) { map_blocks[10][i] = 50 + i; map_blocks[20][i] = 150 - i; }
    std::memcpy(blockA, map_blocks[10], sizeof(blockA));
    std::memcpy(blockB, map_blocks[20], sizeof(blockB));

    std::swap(map_blocks[10], map_blocks[20]);

    bool ok = true;
    if (std::memcmp(map_blocks[10], blockB, sizeof(blockB)) != 0) { ok = false; std::printf("FAIL: block 10 doesn't have block 20's old content\n"); }
    if (std::memcmp(map_blocks[20], blockA, sizeof(blockA)) != 0) { ok = false; std::printf("FAIL: block 20 doesn't have block 10's old content\n"); }
    if (std::memcmp(map_blocks[99], refOther, sizeof(refOther)) != 0) { ok = false; std::printf("FAIL: unrelated block 99 was affected\n"); }

    std::printf(ok ? "OK: block swap exact, no side effects\n" : "FAIL\n");
    return ok ? 0 : 1;
}
