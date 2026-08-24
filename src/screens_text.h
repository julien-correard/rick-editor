// screens_text.h -- texte d'introduction affiche entre les niveaux
// ("map intro text" dans le code d'origine, xrick/src/data/dat_screens.c
// -- SEULE la partie texte a ete portee ici, pas les listes de sprites
// d'animation/steps de la meme intro, qui restent hors-scope pour
// l'instant). Voir src/dat_screens.c pour les 5 tableaux d'octets bruts.
//
// Ce texte n'est PAS rendu avec une police dediee : chaque caractere de
// la chaine est directement un INDICE DE TUILE dans tiles_data[0] (voir
// draw_tile()/draw_tilesSubList() dans draw.c, et draw_tilesBank=0 pour
// GFXST dans screen_introMap()/scr_imap.c) -- c'est-a-dire que la
// "banque 0", que le Tile Editor/Block Editor masquent comme "padding
// inutilise", est en realite la banque de police (glyphes A-Z, chiffres,
// ponctuation, `@` = espace) utilisee pour tout le texte a l'ecran du
// jeu (intros, hall of fame, pause, game over...). Ce fichier ne
// couvre que le texte d'intro de niveau ; les autres ecrans texte
// (dat_screens.c en a d'autres) restent hors-scope.
//
// Format brut (screen_imaptext_*[]) : une suite d'octets -- lettres/
// chiffres/ponctuation ASCII tels quels, `@` (0x40) pour un espace,
// termines par 0xFF (fin de ligne, la "tete d'ecriture" repart au debut
// et descend d'une tuile = 8px) ou 0xFF 0xFF consecutifs (ligne vide :
// draw_tilesSubList() ne dessine rien entre deux 0xFF adjacents, mais
// draw_tilesList() descend quand meme d'une ligne -- c'est un simple
// espacement visuel, pas une pause temporelle malgre les apparences).
// Le tout se termine par un unique 0xFE (fin de texte).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "screens_text_data.h" // U8, SCREEN_IMAPTEXT_COUNT, extern screen_imaptext_*[] (see src/dat_screens.c)
#include "xrick_patch.h"       // find_symbol_file_offset, patch_symbol

// Noms courts pour l'UI, dans le meme ordre que screen_imaptext[] (donc
// que game_map, l'indice utilise par le jeu pour choisir quel texte
// afficher entre deux niveaux).
static const char *SCREEN_IMAPTEXT_LABELS[SCREEN_IMAPTEXT_COUNT] = {
    "Map 0: South America (Amazon)",
    "Map 1: Egypt",
    "Map 2: Castle",
    "Map 3: Missile Base",
    "Map 4: Much Later (Epilogue)",
};

// Cutscene "decor" (background scenery) shown behind Rick on the
// between-levels intro screen -- see the original scr_imap.c's
// drawcenter(): a 6x6 grid of tiles (36 total, 48x48px) from BANK 0
// (the same bank the font above lives in), starting at a different
// tile index per map, walked left-to-right then top-to-bottom (`for i
// in 0..6 { for j in 0..6 { draw_tile(tn++) } }`). Same
// game_map/SCREEN_IMAPTEXT_LABELS order. Confirmed non-overlapping with
// the font's ASCII-driven tile range in the stock data: each 36-tile
// block sits entirely above the highest character code the 5 intro
// texts use ('Z' = 90), except map 0 (7-42), which sits entirely below
// the lowest one used ('.' = 46).
static const int SCREEN_IMAP_DECOR_START_TILE[SCREEN_IMAPTEXT_COUNT] = { 0x07, 0x5B, 0x7F, 0xA3, 0xC7 };
static const int SCREEN_IMAP_DECOR_COLS = 6;
static const int SCREEN_IMAP_DECOR_ROWS = 6;

// Une ligne editable : texte affichable (espaces reels, PAS de `@` --
// la conversion vers/depuis le format brut se fait aux limites, voir
// parseImapText() plus bas) et si une ligne vide doit suivre (double
// 0xFF dans le format brut).
struct ImapTextRow
{
    std::string text;         // caracteres tels qu'affiches (espace reel pour `@`)
    bool blankLineAfter = false; // ligne vide supplementaire apres celle-ci (espacement visuel)
};

struct ImapText
{
    std::vector<ImapTextRow> rows;
};

// Decode un tableau brut (termine par 0xFE) en lignes editables. Miroir
// exact de la logique de draw_tilesSubList()/draw_tilesList() dans
// draw.c : une ligne se termine au premier 0xFF ou 0xFE ; un 0xFF
// immediatement suivi d'un second 0xFF (ligne vide) est absorbe et
// traduit en blankLineAfter=true sur la ligne qui precede.
inline ImapText parseImapText(const U8 *raw)
{
    ImapText t;
    std::string cur;
    const U8 *p = raw;
    for (;;)
    {
        U8 c = *p++;
        if (c == 0xFF || c == 0xFE)
        {
            bool blank = false;
            if (c == 0xFF && *p == 0xFF) { blank = true; p++; }
            t.rows.push_back(ImapTextRow{cur, blank});
            cur.clear();
            if (c == 0xFE) break;
        }
        else
        {
            cur.push_back(c == '@' ? ' ' : (char)c);
        }
    }
    return t;
}

// Les 5 textes par defaut (contenu d'origine du jeu), pour le chargement
// initial et le bouton "Reset to stock" de chaque entree.
inline std::array<ImapText, SCREEN_IMAPTEXT_COUNT> defaultImapTexts()
{
    std::array<ImapText, SCREEN_IMAPTEXT_COUNT> out;
    for (int i = 0; i < SCREEN_IMAPTEXT_COUNT; i++)
        out[i] = parseImapText(screen_imaptext[i]);
    return out;
}

// Noms des symboles ELF portant chaque texte, dans le meme ordre que
// screen_imaptext[]/SCREEN_IMAPTEXT_LABELS[] -- utilise par le patch et
// l'import depuis un binaire xrick ci-dessous.
static const char *SCREEN_IMAPTEXT_SYMBOLS[SCREEN_IMAPTEXT_COUNT] = {
    "screen_imaptext_amazon",
    "screen_imaptext_egypt",
    "screen_imaptext_castle",
    "screen_imaptext_missile",
    "screen_imaptext_muchlater",
};

// Encode les lignes editables au format brut (inverse exact de
// parseImapText() : un 0xFF termine chaque ligne non-finale, un second
// 0xFF immediatement apres si blankLineAfter, et la derniere ligne se
// termine par 0xFE). Ne fait AUCUN padding -- voir
// encodeImapTextPadded() ci-dessous pour ca.
inline std::vector<uint8_t> encodeImapTextRaw(const ImapText &t)
{
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < t.rows.size(); i++)
    {
        const ImapTextRow &row = t.rows[i];
        for (char ch : row.text)
            bytes.push_back((uint8_t)(ch == ' ' ? '@' : ch));
        bool isLast = (i + 1 == t.rows.size());
        bytes.push_back(isLast ? 0xFE : 0xFF);
        if (!isLast && row.blankLineAfter)
            bytes.push_back(0xFF); // ligne vide : un second 0xFF adjacent
    }
    return bytes;
}

// Encode en visant EXACTEMENT targetSize octets -- necessaire pour
// patcher un symbole ELF existant, dont la taille est fixe. En pratique
// `targetSize` est presque toujours 1 octet de PLUS que le contenu
// logique (le compilateur C ajoute un octet NUL implicite a la fin du
// litteral de chaine qui initialise chaque tableau `screen_imaptext_*`
// -- confirme en comparant la taille du symbole ELF a la longueur du
// flux jusqu'au 0xFE). Ces octets en trop ne sont JAMAIS lus par le jeu
// (draw_tilesList() s'arrete au premier 0xFE rencontre), donc on les
// remplit simplement de zeros plutot que d'injecter des espaces
// visibles dans la derniere ligne -- ce qui laisse le texte rendu
// inchange pour toute edition qui tient dans la taille d'origine,
// contrairement a une premiere version de cette fonction qui gonflait
// artificiellement la derniere ligne. Echoue seulement si le contenu
// logique (jusqu'au 0xFE inclus) depasse targetSize.
inline bool encodeImapTextPadded(const ImapText &t, size_t targetSize, std::vector<uint8_t> &out, std::string &err)
{
    if (t.rows.empty()) { err = "Text has no lines"; return false; }
    std::vector<uint8_t> raw = encodeImapTextRaw(t);
    if (raw.size() > targetSize)
    {
        err = "Text is " + std::to_string(raw.size() - targetSize) + " byte(s) too long to fit "
            "the original " + std::to_string(targetSize) + "-byte slot in the binary "
            "(current: " + std::to_string(raw.size()) + " bytes). Shorten a line, or remove/merge "
            "a blank-line spacer, and try again.";
        return false;
    }
    out = raw;
    out.resize(targetSize, 0); // octets restants : jamais atteints par le rendu (apres le 0xFE), peu importe leur valeur
    err.clear();
    return true;
}

// Lit les 5 textes directement depuis les donnees compilees d'un vrai
// binaire xrick, via leurs symboles ELF, dans out[] -- meme logique
// "faire confiance au vrai binaire" que loadTilesFromXrickBinary() dans
// tile_import.h. N'ecrit RIEN dans out si un seul des 5 echoue (tout ou
// rien, comme les autres imports depuis un binaire de cet editeur).
inline bool loadScreenTextsFromXrickBinary(const fs::path &path, std::array<ImapText, SCREEN_IMAPTEXT_COUNT> &out, std::string &err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "Could not open " + path.string(); return false; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (buf.empty()) { err = "File is empty or unreadable"; return false; }

    std::array<ImapText, SCREEN_IMAPTEXT_COUNT> parsed;
    for (int i = 0; i < SCREEN_IMAPTEXT_COUNT; i++)
    {
        size_t off = 0, size = 0;
        if (!find_symbol_file_offset(buf, SCREEN_IMAPTEXT_SYMBOLS[i], off, size, err)) return false;
        if (off >= buf.size()) { err = std::string(SCREEN_IMAPTEXT_SYMBOLS[i]) + " location falls outside the file"; return false; }
        // parseImapText() s'arrete au 0xFE rencontre -- garanti present
        // dans les `size` premiers octets pour un binaire bien forme,
        // donc on peut lui passer un pointeur direct dans buf.
        parsed[i] = parseImapText(buf.data() + off);
    }
    out = parsed;
    err.clear();
    return true;
}

