// Suite du test screens_text : verifie que encodeImapTextRaw() est
// l'inverse exact de parseImapText() (round-trip byte-a-byte sur les 5
// textes stock), et le comportement de encodeImapTextPadded() (pad
// exact, refus si trop long, no-op si deja pile).
#include "screens_text.h"
#include <cstdio>
#include <cstring>

static size_t rawLen(const U8 *raw) { size_t n = 0; while (raw[n-0] != 0xFE) n++; return n + 1; }

int main()
{
    bool ok = true;

    // --- Round-trip encode(decode(x)) == x, pour les 5 textes stock ---
    for (int i = 0; i < SCREEN_IMAPTEXT_COUNT; i++)
    {
        ImapText t = parseImapText(screen_imaptext[i]);
        std::vector<uint8_t> encoded = encodeImapTextRaw(t);
        size_t origLen = rawLen(screen_imaptext[i]);
        if (encoded.size() != origLen || std::memcmp(encoded.data(), screen_imaptext[i], origLen) != 0)
        {
            ok = false;
            std::printf("FAIL: text %d (%s) doesn't round-trip: got %zu bytes, expected %zu\n",
                        i, SCREEN_IMAPTEXT_SYMBOLS[i], encoded.size(), origLen);
        }
    }
    std::printf(ok ? "OK: all 5 texts round-trip byte-exact through encode(decode(x))\n" : "FAIL round-trip\n");

    // --- Padding exact : texte deliberement raccourci, complete jusqu'a la taille cible ---
    ImapText t = parseImapText(screen_imaptext_amazon);
    size_t target = rawLen(screen_imaptext_amazon);
    t.rows.back().text = "?"; // ligne tres courte -> encodage plus petit que target
    std::vector<uint8_t> padded;
    std::string err;
    bool padOk = encodeImapTextPadded(t, target, padded, err);
    if (!padOk || padded.size() != target) { ok = false; std::printf("FAIL padding: ok=%d size=%zu target=%zu err=%s\n", padOk, padded.size(), target, err.c_str()); }
    else std::printf("OK: padded to exactly %zu bytes\n", padded.size());

    // --- Refus si trop long ---
    ImapText t2 = parseImapText(screen_imaptext_amazon);
    t2.rows[0].text = std::string(200, 'X'); // ligne beaucoup trop longue
    std::vector<uint8_t> tooLong;
    std::string err2;
    bool shouldFail = encodeImapTextPadded(t2, target, tooLong, err2);
    if (shouldFail) { ok = false; std::printf("FAIL: expected encodeImapTextPadded to refuse an oversized text\n"); }
    else std::printf("OK: correctly refused oversized text (%s)\n", err2.c_str());

    // --- Deja pile la bonne taille : no-op ---
    ImapText t3 = parseImapText(screen_imaptext_amazon);
    std::vector<uint8_t> exact;
    std::string err3;
    bool exactOk = encodeImapTextPadded(t3, target, exact, err3);
    if (!exactOk || exact.size() != target) { ok = false; std::printf("FAIL exact-size case\n"); }
    else std::printf("OK: exact-size text encodes unchanged\n");

    std::printf(ok ? "ALL OK\n" : "SOME FAILED\n");
    return ok ? 0 : 1;
}
