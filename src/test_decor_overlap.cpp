// Verifie que les 5 blocs de decor de cinematique (36 tuiles chacun,
// banque 0) ne chevauchent jamais les glyphes de police reellement
// utilises par les 5 textes d'intro (meme banque 0) -- confirme
// l'affirmation du commentaire dans screens_text.h par une verification
// programmatique plutot qu'une simple inspection visuelle des plages.
#include "screens_text.h"
#include <cstdio>
#include <set>

int main()
{
    std::set<int> usedGlyphs;
    for (int i = 0; i < SCREEN_IMAPTEXT_COUNT; i++)
    {
        const U8 *raw = screen_imaptext[i];
        for (const U8 *p = raw; *p != 0xFE; p++)
            if (*p != 0xFF) usedGlyphs.insert((int)*p);
    }
    std::printf("Glyph tiles actually used by the 5 stock texts: %zu distinct codes\n", usedGlyphs.size());

    std::set<int> decorTiles;
    for (int m = 0; m < SCREEN_IMAPTEXT_COUNT; m++)
    {
        int start = SCREEN_IMAP_DECOR_START_TILE[m];
        for (int t = start; t < start + SCREEN_IMAP_DECOR_COLS * SCREEN_IMAP_DECOR_ROWS; t++)
            decorTiles.insert(t);
    }
    std::printf("Decor tiles across all 5 maps: %zu distinct indices\n", decorTiles.size());

    bool ok = true;
    for (int t : decorTiles)
        if (usedGlyphs.count(t))
        { ok = false; std::printf("OVERLAP at tile %d (0x%02X)\n", t, t); }

    std::printf(ok ? "OK: no overlap between decor tiles and used font glyphs\n" : "FAIL: overlap(s) found above\n");
    return ok ? 0 : 1;
}
