// test_xrick_patch.cpp -- exercises xrick_patch.h against a real xrick
// ELF binary (path given on the command line). Not part of the CMake
// build; a manual diagnostic tool.
#include <cstdio>
#include "xrick_patch.h"

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: %s <path-to-xrick-binary>\n", argv[0]); return 2; }

    // Modifie une valeur reconnaissable en memoire avant le patch.
    int original0 = map_bnums[0];
    map_bnums[0] = 42;
    map_bnums[MAP_COUNT - 1] = 7;

    PatchResult r = patchXrickBinary(argv[1]);
    std::printf("ok=%d msg=%s\n", r.ok, r.message.c_str());
    if (!r.ok) return 1;

    // Relit le fichier patche et verifie que les octets attendus y sont.
    size_t off = 0, size = 0; std::string err;
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> orig((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!elf32_find_symbol_file_offset(orig, "map_bnums", off, size, err))
    {
        std::printf("FAIL: could not re-locate symbol: %s\n", err.c_str());
        return 1;
    }

    std::ifstream patched(r.outputPath, std::ios::binary);
    std::vector<uint8_t> pbuf((std::istreambuf_iterator<char>(patched)), std::istreambuf_iterator<char>());

    bool fail = false;
    if (pbuf[off] != 42) { std::printf("FAIL: byte 0 = %d, expected 42\n", pbuf[off]); fail = true; }
    if (pbuf[off + MAP_COUNT - 1] != 7) { std::printf("FAIL: last byte = %d, expected 7\n", pbuf[off + MAP_COUNT - 1]); fail = true; }

    // Verifie que le reste du fichier (hors map_bnums) est rigoureusement inchange.
    if (pbuf.size() != orig.size()) { std::printf("FAIL: file size changed (%zu vs %zu)\n", pbuf.size(), orig.size()); fail = true; }
    else
    {
        size_t diffCountOutside = 0;
        for (size_t i = 0; i < orig.size(); i++)
        {
            if (i >= off && i < off + (size_t)MAP_COUNT) continue;
            if (orig[i] != pbuf[i]) diffCountOutside++;
        }
        if (diffCountOutside != 0) { std::printf("FAIL: %zu byte(s) changed outside map_bnums!\n", diffCountOutside); fail = true; }
    }

    // Verifie aussi les valeurs intermediaires (round trip complet).
    size_t mismatches = 0;
    for (int i = 1; i < MAP_COUNT - 1; i++)
        if (pbuf[off + i] != (uint8_t)std::clamp(map_bnums[i], 0, 255)) mismatches++;
    if (mismatches != 0) { std::printf("FAIL: %zu mismatched bytes in the body\n", mismatches); fail = true; }

    map_bnums[0] = original0; // restore (harmless, process ends anyway)

    if (fail) { std::printf("TEST FAILED\n"); return 1; }
    std::printf("OK: patch applied correctly, rest of file untouched, output at %s\n", r.outputPath.string().c_str());
    return 0;
}
