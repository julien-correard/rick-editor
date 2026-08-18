// Test round-trip du format .map RKM9 (mapTexts) : sauvegarde,
// modification en memoire, rechargement, verification -- y compris un
// texte dont une ligne a ete allongee/raccourcie et une ligne
// supprimee/ajoutee (verifie que le format prefixe par longueur gere
// bien le variable-length, contrairement aux tableaux a taille fixe).
#define STB_IMAGE_IMPLEMENTATION
#include "mapfile.h"
#include "xrick_marks.h"
#include "sprite_import.h"
#include <cstdio>
#include <cstring>

int main()
{
    std::string err;
    ConnectionsData conn = defaultConnections();
    MarksData marks = defaultMarks();
    EflgData eflg = defaultEflg();
    std::array<ImapText, SCREEN_IMAPTEXT_COUNT> texts = defaultImapTexts();

    // Reference pour un texte non touche.
    ImapText refCastle = texts[2];

    // Modifications sur le texte "amazon" (indice 0) : ligne raccourcie,
    // une ligne ajoutee, une ligne supprimee, blankLineAfter change.
    texts[0].rows[0].text = "HELLO WORLD";
    texts[0].rows[0].blankLineAfter = true;
    texts[0].rows.push_back(ImapTextRow{"NEW LAST LINE", false});
    texts[0].rows.erase(texts[0].rows.begin() + 3);

    fs::path tmp = fs::temp_directory_path() / "rickeditor_test_rkm9.map";
    if (!saveMapFileWithSprites(tmp, conn, marks, eflg, texts, err)) { std::printf("FAIL save: %s\n", err.c_str()); return 1; }

    // On efface tout pour s'assurer que le rechargement ecrase bien.
    std::array<ImapText, SCREEN_IMAPTEXT_COUNT> before = texts;
    texts[0] = ImapText{};
    texts[2] = ImapText{};

    ConnectionsData conn2; MarksData marks2; EflgData eflg2;
    std::array<ImapText, SCREEN_IMAPTEXT_COUNT> texts2;
    if (!loadMapFileWithSprites(tmp, conn2, marks2, eflg2, texts2, err)) { std::printf("FAIL load: %s\n", err.c_str()); return 1; }

    bool ok = true;
    if (texts2[0].rows.size() != before[0].rows.size()) { ok = false; std::printf("FAIL: row count mismatch (%zu vs %zu)\n", texts2[0].rows.size(), before[0].rows.size()); }
    else
    {
        for (size_t i = 0; i < texts2[0].rows.size(); i++)
        {
            if (texts2[0].rows[i].text != before[0].rows[i].text || texts2[0].rows[i].blankLineAfter != before[0].rows[i].blankLineAfter)
            { ok = false; std::printf("FAIL: row %zu mismatch: \"%s\" (blank=%d) vs \"%s\" (blank=%d)\n",
                        i, texts2[0].rows[i].text.c_str(), texts2[0].rows[i].blankLineAfter,
                        before[0].rows[i].text.c_str(), before[0].rows[i].blankLineAfter); }
        }
    }
    // Texte non touche (castle, indice 2) doit rester identique.
    if (texts2[2].rows.size() != refCastle.rows.size()) { ok = false; std::printf("FAIL: untouched castle text row count changed\n"); }
    else for (size_t i = 0; i < texts2[2].rows.size(); i++)
        if (texts2[2].rows[i].text != refCastle.rows[i].text) { ok = false; std::printf("FAIL: untouched castle text row %zu changed\n", i); }

    std::printf(ok ? "OK: RKM9 round-trip (mapTexts, variable-length) exact\n" : "FAIL: mismatch(es) above\n");
    return ok ? 0 : 1;
}
