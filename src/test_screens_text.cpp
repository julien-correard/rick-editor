// Test de parseImapText() : verifie le decodage du texte "amazon"
// (premiere ligne = "SOUTH AMERICA 1945"), le nombre de lignes, la
// detection des lignes vides (double 0xFF), et que les 5 textes se
// decodent tous sans lever d'exception ni boucler indefiniment.
#include "screens_text.h"
#include <cstdio>

int main()
{
    bool ok = true;

    ImapText amazon = parseImapText(screen_imaptext_amazon);
    std::printf("amazon: %d lines\n", (int)amazon.rows.size());
    if (amazon.rows.empty() || amazon.rows[0].text != "     SOUTH AMERICA 1945       ")
    {
        ok = false;
        std::printf("FAIL: first line = \"%s\" (expected \"     SOUTH AMERICA 1945       \")\n",
                    amazon.rows.empty() ? "<none>" : amazon.rows[0].text.c_str());
    }
    // Ligne 5 (indice 4) = "TRIBE." avec beaucoup de @ = espaces, ET blankLineAfter=true (double 0xFF)
    if (amazon.rows.size() < 5 || !amazon.rows[4].blankLineAfter)
    {
        ok = false;
        std::printf("FAIL: expected row 4 (TRIBE.) to have blankLineAfter=true\n");
    }
    if (amazon.rows.size() < 5 || amazon.rows[4].text.find("TRIBE.") == std::string::npos)
    {
        ok = false;
        std::printf("FAIL: row 4 should contain \"TRIBE.\"\n");
    }
    // Derniere ligne ne doit pas avoir blankLineAfter (c'est la fin, 0xFE)
    if (amazon.rows.back().blankLineAfter)
    {
        ok = false;
        std::printf("FAIL: last row shouldn't have blankLineAfter\n");
    }
    // Aucune ligne ne doit contenir de '@' residuel (tout converti en espace)
    for (auto &r : amazon.rows)
        if (r.text.find('@') != std::string::npos)
        { ok = false; std::printf("FAIL: '@' leaked into decoded text: \"%s\"\n", r.text.c_str()); }

    // Les 5 textes doivent se decoder sans souci (pas de plantage/boucle infinie).
    auto all = defaultImapTexts();
    for (int i = 0; i < SCREEN_IMAPTEXT_COUNT; i++)
    {
        std::printf("text %d (%s): %d lines\n", i, SCREEN_IMAPTEXT_LABELS[i], (int)all[i].rows.size());
        if (all[i].rows.empty()) { ok = false; std::printf("FAIL: text %d has no rows\n", i); }
    }

    std::printf(ok ? "OK: parseImapText decodes correctly\n" : "FAIL\n");
    return ok ? 0 : 1;
}
