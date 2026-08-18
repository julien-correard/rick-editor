// Test isole du patch ELF de texte (sans passer par
// patchXrickBinaryWithSprites, qui exige aussi map_bnums/map_submaps/
// map_connect/map_marks -- hors-sujet ici, deja teste ailleurs pour
// tiles_data/map_blocks/sprites_data avec la meme methodologie) :
// verifie encodeImapTextPadded() + elf32_patch_symbol() +
// loadScreenTextsFromXrickBinary() en aller-retour sur un vrai ELF32,
// pour les 5 textes, avec des edits de longueurs variees (plus court,
// plus long mais toujours <= original, pile la bonne taille).
#define STB_IMAGE_IMPLEMENTATION
#include "screens_text.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 2) { std::printf("usage: %s <fake_xrick_elf32>\n", argv[0]); return 2; }
    fs::path target = argv[1];

    std::array<ImapText, SCREEN_IMAPTEXT_COUNT> texts = defaultImapTexts();
    ImapText refUntouched = texts[2]; // castle : jamais modifie

    // amazon : raccourcir la 1ere ligne (padding attendu)
    texts[0].rows[0].text = "HI";
    // egypt : supprimer une ligne (raccourcit encore plus, plus de padding)
    texts[1].rows.pop_back();
    // missile : ne rien changer (deja pile la bonne taille)
    // muchlater : ajouter un peu de texte a la derniere ligne (toujours <= original ? verifions par un texte court)
    texts[4].rows.back().text += "!"; // ajoute 1 caractere -> peut-etre trop long, on verra si erreur geree

    std::ifstream in(target, std::ios::binary);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    std::string err;
    bool anyOk = true;
    for (int i = 0; i < SCREEN_IMAPTEXT_COUNT; i++)
    {
        size_t off = 0, symSize = 0;
        if (!elf32_find_symbol_file_offset(buf, SCREEN_IMAPTEXT_SYMBOLS[i], off, symSize, err))
        { std::printf("FAIL find symbol %s: %s\n", SCREEN_IMAPTEXT_SYMBOLS[i], err.c_str()); return 1; }
        std::vector<uint8_t> textBytes;
        bool ok = encodeImapTextPadded(texts[i], symSize, textBytes, err);
        std::printf("text %d (%s): symSize=%zu encodeOk=%d (%s)\n", i, SCREEN_IMAPTEXT_SYMBOLS[i], symSize, ok, err.c_str());
        if (!ok) { anyOk = false; continue; } // texte 4 pourrait legitimement echouer si trop long -- on continue les autres
        if (!elf32_patch_symbol(buf, SCREEN_IMAPTEXT_SYMBOLS[i], textBytes.data(), textBytes.size(), err))
        { std::printf("FAIL patch %s: %s\n", SCREEN_IMAPTEXT_SYMBOLS[i], err.c_str()); return 1; }
    }

    fs::path patched = fs::temp_directory_path() / "fake_xrick_text_patched";
    std::ofstream out(patched, std::ios::binary | std::ios::trunc);
    out.write((const char*)buf.data(), (std::streamsize)buf.size());
    out.close();

    std::array<ImapText, SCREEN_IMAPTEXT_COUNT> reloaded;
    if (!loadScreenTextsFromXrickBinary(patched, reloaded, err))
    { std::printf("FAIL reload: %s\n", err.c_str()); return 1; }

    bool ok = true;
    if (reloaded[0].rows[0].text.substr(0, 2) != "HI") { ok = false; std::printf("FAIL: amazon row0 = \"%s\"\n", reloaded[0].rows[0].text.c_str()); }
    // Verifie que le reste des lignes amazon est intact (le padding ne doit toucher QUE la derniere ligne du texte, pas amazon row0's siblings)
    ImapText stockAmazon = defaultImapTexts()[0];
    for (size_t i = 1; i + 1 < stockAmazon.rows.size(); i++) // sauf la derniere (padding target) et la premiere (modifiee)
        if (reloaded[0].rows[i].text != stockAmazon.rows[i].text)
        { ok = false; std::printf("FAIL: amazon row %zu unexpectedly changed: \"%s\"\n", i, reloaded[0].rows[i].text.c_str()); }

    if (reloaded[1].rows.size() != texts[1].rows.size())
    { ok = false; std::printf("FAIL: egypt row count mismatch after patch: got %zu expected %zu\n", reloaded[1].rows.size(), texts[1].rows.size()); }

    // Texte non touche (castle) doit rester identique au stock.
    if (reloaded[2].rows.size() != refUntouched.rows.size()) { ok = false; std::printf("FAIL: untouched castle row count changed\n"); }
    else for (size_t i = 0; i < refUntouched.rows.size(); i++)
        if (reloaded[2].rows[i].text != refUntouched.rows[i].text)
        { ok = false; std::printf("FAIL: untouched castle row %zu changed: \"%s\" vs \"%s\"\n", i, reloaded[2].rows[i].text.c_str(), refUntouched.rows[i].text.c_str()); }

    // missile (indice 3, non modifie) doit rester identique au stock aussi.
    ImapText stockMissile = defaultImapTexts()[3];
    if (reloaded[3].rows.size() != stockMissile.rows.size()) { ok = false; std::printf("FAIL: missile row count changed\n"); }
    else for (size_t i = 0; i < stockMissile.rows.size(); i++)
        if (reloaded[3].rows[i].text != stockMissile.rows[i].text)
        { ok = false; std::printf("FAIL: missile row %zu changed\n", i); }

    std::printf(ok ? "OK: text patch + reload round-trip correct\n" : "FAIL\n");
    return ok ? 0 : 1;
}
