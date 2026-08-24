// xrick_patch.h -- patches a compiled xrick executable in place,
// injecting the level currently open in the editor (map_bnums) at its
// original location, found dynamically via the ELF symbol table or,
// for stripped PE32 binaries (Windows), via known byte-pattern scanning.
//
// Format note: in the real xrick engine, map_bnums is a U8[] (one byte
// per block index, 0-255) -- unlike our editor's `int map_bnums[]`
// (4 bytes/entry, same value range, kept from the original Windows tool).
// We only ever WRITE into the target file; the editor's own in-memory
// array and .map format are untouched by this.
#pragma once

#include <elf.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>

#include "mapdata.h"
#include "mapfile.h" // MAP_COUNT

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// ELF32 symbol lookup
// ---------------------------------------------------------------------------

inline bool elf32_find_symbol_file_offset(const std::vector<uint8_t> &buf,
                                           const char *symbolName,
                                           size_t &fileOffset, size_t &symSize,
                                           std::string &err)
{
    if (buf.size() < sizeof(Elf32_Ehdr) || std::memcmp(buf.data(), ELFMAG, SELFMAG) != 0)
    {
        err = "Not an ELF file"; return false;
    }
    const auto *eh = reinterpret_cast<const Elf32_Ehdr*>(buf.data());
    if (eh->e_ident[EI_CLASS] != ELFCLASS32)
    {
        err = "Not a 32-bit ELF";
        return false;
    }
    if (eh->e_shoff == 0 || eh->e_shnum == 0)
    {
        err = "No section headers found"; return false;
    }

    auto shAt = [&](int i) -> const Elf32_Shdr* {
        size_t off = eh->e_shoff + (size_t)i * eh->e_shentsize;
        if (off + sizeof(Elf32_Shdr) > buf.size()) return nullptr;
        return reinterpret_cast<const Elf32_Shdr*>(buf.data() + off);
    };

    const Elf32_Shdr *symtabSh = nullptr;
    for (int i = 0; i < eh->e_shnum; i++)
    {
        const Elf32_Shdr *sh = shAt(i);
        if (sh && sh->sh_type == SHT_SYMTAB) { symtabSh = sh; break; }
    }
    if (!symtabSh)
    {
        err = "No symbol table in this binary (it looks stripped)";
        return false;
    }
    const Elf32_Shdr *strtabSh = shAt(symtabSh->sh_link);
    if (!strtabSh) { err = "Malformed symbol table (missing string table)"; return false; }

    const char *strtab = reinterpret_cast<const char*>(buf.data() + strtabSh->sh_offset);
    size_t strtabSize = strtabSh->sh_size;
    int nsyms = symtabSh->sh_size / sizeof(Elf32_Sym);
    const auto *syms = reinterpret_cast<const Elf32_Sym*>(buf.data() + symtabSh->sh_offset);

    for (int i = 0; i < nsyms; i++)
    {
        const Elf32_Sym &sym = syms[i];
        if (sym.st_name == 0 || sym.st_name >= strtabSize) continue;
        const char *name = strtab + sym.st_name;
        if (std::strcmp(name, symbolName) != 0) continue;
        if (sym.st_shndx == SHN_UNDEF) continue;

        const Elf32_Shdr *containingSh = shAt(sym.st_shndx);
        if (!containingSh) { err = "Symbol found but its section is invalid"; return false; }
        if (containingSh->sh_type == SHT_NOBITS) { err = "Symbol is in .bss (no data stored in file)"; return false; }

        fileOffset = containingSh->sh_offset + (sym.st_value - containingSh->sh_addr);
        symSize = sym.st_size;
        return true;
    }

    err = std::string("Symbol '") + symbolName + "' not found in this ELF32 binary";
    return false;
}

// ---------------------------------------------------------------------------
// ELF64 symbol lookup (for 64-bit PIE executables)
// ---------------------------------------------------------------------------

inline bool elf64_find_symbol_file_offset(const std::vector<uint8_t> &buf,
                                           const char *symbolName,
                                           size_t &fileOffset, size_t &symSize,
                                           std::string &err)
{
    if (buf.size() < sizeof(Elf64_Ehdr) || std::memcmp(buf.data(), ELFMAG, SELFMAG) != 0)
    {
        err = "Not an ELF file"; return false;
    }
    const auto *eh = reinterpret_cast<const Elf64_Ehdr*>(buf.data());
    if (eh->e_ident[EI_CLASS] != ELFCLASS64)
    {
        err = "Not a 64-bit ELF";
        return false;
    }
    if (eh->e_shoff == 0 || eh->e_shnum == 0)
    {
        err = "No section headers found"; return false;
    }

    auto shAt = [&](int i) -> const Elf64_Shdr* {
        size_t off = eh->e_shoff + (size_t)i * eh->e_shentsize;
        if (off + sizeof(Elf64_Shdr) > buf.size()) return nullptr;
        return reinterpret_cast<const Elf64_Shdr*>(buf.data() + off);
    };

    const Elf64_Shdr *symtabSh = nullptr;
    for (int i = 0; i < eh->e_shnum; i++)
    {
        const Elf64_Shdr *sh = shAt(i);
        if (sh && sh->sh_type == SHT_SYMTAB) { symtabSh = sh; break; }
    }
    if (!symtabSh)
    {
        err = "No symbol table in this binary (it looks stripped)";
        return false;
    }
    const Elf64_Shdr *strtabSh = shAt(symtabSh->sh_link);
    if (!strtabSh) { err = "Malformed symbol table (missing string table)"; return false; }

    const char *strtab = reinterpret_cast<const char*>(buf.data() + strtabSh->sh_offset);
    size_t strtabSize = strtabSh->sh_size;
    int nsyms = symtabSh->sh_size / sizeof(Elf64_Sym);
    const auto *syms = reinterpret_cast<const Elf64_Sym*>(buf.data() + symtabSh->sh_offset);

    for (int i = 0; i < nsyms; i++)
    {
        const Elf64_Sym &sym = syms[i];
        if (sym.st_name == 0 || sym.st_name >= strtabSize) continue;
        const char *name = strtab + sym.st_name;
        if (std::strcmp(name, symbolName) != 0) continue;
        if (sym.st_shndx == SHN_UNDEF) continue;

        const Elf64_Shdr *containingSh = shAt(sym.st_shndx);
        if (!containingSh) { err = "Symbol found but its section is invalid"; return false; }
        if (containingSh->sh_type == SHT_NOBITS) { err = "Symbol is in .bss (no data stored in file)"; return false; }

        fileOffset = containingSh->sh_offset + (sym.st_value - containingSh->sh_addr);
        symSize = sym.st_size;
        return true;
    }

    err = std::string("Symbol '") + symbolName + "' not found in this ELF64 binary";
    return false;
}

// ---------------------------------------------------------------------------
// PE32 pattern-scanning lookup (for stripped Windows xrick.exe)
// ---------------------------------------------------------------------------

struct PE32PatternEntry
{
    const char *name;
    const uint8_t *pattern;
    size_t patternLen;
    size_t expectedSize;    // total symbol size in the binary
    ptrdiff_t patternOffset; // offset from pattern match to symbol start
};

// Forward declaration -- full implementation is after the generic wrappers.
inline bool pe32_find_symbol_file_offset(const std::vector<uint8_t> &buf,
                                          const char *symbolName,
                                          size_t &fileOffset, size_t &symSize,
                                          std::string &err);

// ---------------------------------------------------------------------------
// Generic wrappers: auto-detect ELF32 vs PE32
// ---------------------------------------------------------------------------

inline bool find_symbol_file_offset(const std::vector<uint8_t> &buf,
                                     const char *symbolName,
                                     size_t &fileOffset, size_t &symSize,
                                     std::string &err)
{
    // Try ELF32 first
    std::string elf32Err;
    if (elf32_find_symbol_file_offset(buf, symbolName, fileOffset, symSize, elf32Err))
        return true;

    // Try ELF64
    std::string elf64Err;
    if (elf64_find_symbol_file_offset(buf, symbolName, fileOffset, symSize, elf64Err))
        return true;

    // Try PE32 pattern scan
    std::string peErr;
    if (pe32_find_symbol_file_offset(buf, symbolName, fileOffset, symSize, peErr))
        return true;

    err = elf32Err + " / " + elf64Err + " / " + peErr;
    return false;
}

inline bool patch_symbol(std::vector<uint8_t> &buf, const char *symbolName,
                          const void *newData, size_t newSize, std::string &err)
{
    size_t off = 0, size = 0;
    if (!find_symbol_file_offset(buf, symbolName, off, size, err)) return false;
    if (size != newSize)
    {
        err = std::string(symbolName) + " has a different size (" + std::to_string(size)
            + " bytes, expected " + std::to_string(newSize) + ") -- likely an incompatible xrick build.";
        return false;
    }
    if (off + newSize > buf.size()) { err = std::string(symbolName) + " location falls outside the file"; return false; }
    std::memcpy(buf.data() + off, newData, newSize);
    return true;
}

// ---------------------------------------------------------------------------
// PE32 implementation: pattern-scanning in .data section
// ---------------------------------------------------------------------------

// Pattern bytes extracted from the original xrick source (dat_maps.c,
// dat_ememies.c, etc.) and verified to appear exactly once in the
// .data section of the Windows xrick.exe (PE32, stripped).
// Each entry stores: pattern, its length, the full symbol size, and
// the byte offset from the pattern match to the symbol's start.
// If patternOffset == 0, the pattern IS the start of the symbol.

// map_bnums: at offset +256 a unique 16-byte chunk (verified unique)
static const uint8_t pat_map_bnums[] = {
    0x50, 0x42, 0x6e, 0x40, 0x40, 0x71, 0x51, 0x54,
    0x54, 0x43, 0x40, 0x40, 0x72, 0x52, 0x46, 0x46
};
// map_connect: first 6 bytes are unique
static const uint8_t pat_map_connect[] = {
    0x01, 0x18, 0xff, 0x00, 0x00, 0x38
};
// map_submaps: first 16 bytes
static const uint8_t pat_map_submaps[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x78, 0x00, 0x03, 0x00, 0x05, 0x00
};
// map_eflg_c: first 16 bytes
static const uint8_t pat_map_eflg_c[] = {
    0x4d, 0x00, 0x0e, 0x02, 0x04, 0x04, 0x57, 0x08,
    0x08, 0x18, 0x03, 0x68, 0x3b, 0x48, 0x04, 0x80
};
// map_marks: 16-byte unique chunk at offset +506 (avoids false positive
// where the first 20 bytes are duplicated elsewhere in PE .data)
static const uint8_t pat_map_marks[] = {
    0x17, 0x01, 0xe0, 0xe0, 0x18, 0x1d, 0x88, 0x81,
    0x60, 0x18, 0x1e, 0x88, 0x89, 0xa0, 0x20, 0x07
};
// map_maps: first 8 bytes (x=8, y=0x8b, row=8, submap=0 -- universal)
static const uint8_t pat_map_maps[] = {
    0x08, 0x00, 0x8b, 0x00, 0x08, 0x00, 0x00, 0x00
};
// tiles_data: tile[1] (at offset +32) -- unique pixel pattern
static const uint8_t pat_tiles_data[] = {
    0x00, 0xdc, 0x0c, 0x00, 0x00, 0xfd, 0x0d, 0x00,
    0x00, 0xfd, 0x0d, 0x00, 0x00, 0xfd, 0x0d, 0x00,
    0x00, 0xfd, 0x0d, 0x00, 0x00, 0xec, 0x0c, 0x00,
    0x00, 0xfd, 0x0d, 0x00
};
// map_blocks: block[1] (at offset +16) -- unique tile-index pattern
static const uint8_t pat_map_blocks[] = {
    0x29, 0x2a, 0x2d, 0x2e, 0x2b, 0x2c, 0x2f, 0x30,
    0x22, 0x31, 0x32, 0x20, 0x28, 0x33, 0x34, 0x26
};
// sprites_data: sprite[101] rows 2-4 first 32 bytes (dense unique sprite)
static const uint8_t pat_sprites_data[] = {
    0xdd, 0xed, 0xee, 0xee, 0xdc, 0xdd, 0xdd, 0xdd,
    0xdc, 0xdd, 0xdd, 0xfe, 0xdc, 0xdd, 0xdd, 0xfe,
    0xde, 0xdd, 0xed, 0xfe, 0xcc, 0xdd, 0xdd, 0xdd,
    0xdc, 0xdc, 0xdc, 0xfd, 0xcc, 0xcc, 0xcc, 0xdc
};
// screen_imaptext_amazon: first 16 bytes (unique encoded text)
static const uint8_t pat_imaptext_amazon[] = {
    0x40, 0x40, 0x40, 0x40, 0x40, 0x53, 0x4f, 0x55,
    0x54, 0x48, 0x40, 0x41, 0x4d, 0x45, 0x52, 0x49
};
// screen_imaptext_egypt
static const uint8_t pat_imaptext_egypt[] = {
    0x40, 0x40, 0x40, 0x40, 0x45, 0x47, 0x59, 0x50,
    0x54, 0x2c, 0x40, 0x53, 0x4f, 0x4d, 0x45, 0x54
};
// screen_imaptext_castle
static const uint8_t pat_imaptext_castle[] = {
    0x40, 0x40, 0x40, 0x40, 0x45, 0x55, 0x52, 0x4f,
    0x50, 0x45, 0x2c, 0x40, 0x4c, 0x41, 0x54, 0x45
};
// screen_imaptext_missile
static const uint8_t pat_imaptext_missile[] = {
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x45, 0x55,
    0x52, 0x4f, 0x50, 0x45, 0x2c, 0x40, 0x45, 0x56
};
// screen_imaptext_muchlater
static const uint8_t pat_imaptext_muchlater[] = {
    0x40, 0x40, 0x40, 0x4c, 0x4f, 0x4e, 0x44, 0x4f,
    0x4e, 0x2c, 0x40, 0x4d, 0x55, 0x43, 0x48, 0x2c
};

static const PE32PatternEntry pe32_patterns[] = {
    { "map_bnums",             pat_map_bnums,       sizeof(pat_map_bnums),       0x1fd8,  256 },
    { "map_connect",           pat_map_connect,     sizeof(pat_map_connect),     0x264,     0 },
    { "map_submaps",           pat_map_submaps,     sizeof(pat_map_submaps),     0x178,     0 },
    { "map_eflg_c",            pat_map_eflg_c,      sizeof(pat_map_eflg_c),      0x20,      0 },
    { "map_marks",             pat_map_marks,       sizeof(pat_map_marks),       0xa37,   506 },
    { "map_maps",              pat_map_maps,        sizeof(pat_map_maps),        0x3c,      0 },
    { "tiles_data",            pat_tiles_data,      sizeof(pat_tiles_data),      0x6000,   32 },
    { "map_blocks",            pat_map_blocks,      sizeof(pat_map_blocks),      0x1000,   16 },
    { "sprites_data",          pat_sprites_data,    sizeof(pat_sprites_data),    0x11790, 0x84b0 },
    { "screen_imaptext_amazon",    pat_imaptext_amazon,    sizeof(pat_imaptext_amazon),    0x139, 0 },
    { "screen_imaptext_egypt",     pat_imaptext_egypt,     sizeof(pat_imaptext_egypt),     0x11a, 0 },
    { "screen_imaptext_castle",    pat_imaptext_castle,    sizeof(pat_imaptext_castle),    0x11a, 0 },
    { "screen_imaptext_missile",   pat_imaptext_missile,   sizeof(pat_imaptext_missile),   0xfb,  0 },
    { "screen_imaptext_muchlater", pat_imaptext_muchlater, sizeof(pat_imaptext_muchlater), 0x11a, 0 },
};

inline bool pe32_find_symbol_file_offset(const std::vector<uint8_t> &buf,
                                          const char *symbolName,
                                          size_t &fileOffset, size_t &symSize,
                                          std::string &err)
{
    // Quick reject: not a PE32 file
    if (buf.size() < 0x40 || buf[0] != 'M' || buf[1] != 'Z')
    {
        err = "Not a PE32 file"; return false;
    }

    // Parse PE header to find .data section bounds
    uint32_t peOff = *reinterpret_cast<const uint32_t*>(buf.data() + 0x3C);
    if (peOff + 24 > buf.size() || buf[peOff] != 'P' || buf[peOff+1] != 'E')
    {
        err = "Invalid PE signature"; return false;
    }
    uint16_t numSections = *reinterpret_cast<const uint16_t*>(buf.data() + peOff + 6);
    uint16_t optHeaderSize = *reinterpret_cast<const uint16_t*>(buf.data() + peOff + 20);
    size_t sectionTableOff = peOff + 24 + optHeaderSize;

    size_t dataStart = 0, dataSize = 0;
    for (int i = 0; i < numSections; i++)
    {
        size_t sh = sectionTableOff + (size_t)i * 40;
        if (sh + 40 > buf.size()) break;
        if (std::memcmp(buf.data() + sh, ".data", 5) == 0)
        {
            dataStart = *reinterpret_cast<const uint32_t*>(buf.data() + sh + 20);
            dataSize = *reinterpret_cast<const uint32_t*>(buf.data() + sh + 16);
            break;
        }
    }
    if (dataStart == 0 || dataSize == 0)
    {
        err = "No .data section found in PE32 binary"; return false;
    }

    // Search the pattern table
    for (const auto &entry : pe32_patterns)
    {
        if (std::strcmp(entry.name, symbolName) != 0) continue;

        size_t searchEnd = (dataStart + dataSize > buf.size()) ? buf.size() : dataStart + dataSize;
        size_t pos = dataStart;
        while (pos + entry.patternLen <= searchEnd)
        {
            if (std::memcmp(buf.data() + pos, entry.pattern, entry.patternLen) == 0)
            {
                size_t symStart = pos - entry.patternOffset;
                // Bounds check: symbol must stay within the file
                if (symStart + entry.expectedSize > buf.size())
                {
                    err = std::string(symbolName) + " pattern matched but symbol overflows file";
                    return false;
                }
                fileOffset = symStart;
                symSize = entry.expectedSize;
                return true;
            }
            pos++;
        }

        err = std::string("Pattern for '") + symbolName + "' not found in PE32 .data section";
        return false;
    }

    err = std::string("Unknown symbol '") + symbolName + "' for PE32 pattern scanning";
    return false;
}

// Converts the editor's in-memory map_bnums (int[], clamped to 0-255)
// into the packed byte form the real engine uses (U8[]).
inline std::vector<uint8_t> map_bnums_as_bytes()
{
    std::vector<uint8_t> out(MAP_COUNT);
    for (int i = 0; i < MAP_COUNT; i++)
        out[i] = (uint8_t)std::clamp(map_bnums[i], 0, 255);
    return out;
}

// map_blocks[256][16] is `int` in this editor's own in-memory copy (see
// mapdata.h) for arithmetic convenience, but the real engine stores each
// entry as a single byte (block_t = U8[16] in the original source,
// xrick/include/maps.h -- confirmed against dat_maps.c's initializer,
// every value 0x00-0xff) -- same "widened for the editor, packed back
// down for the binary" pattern as map_bnums_as_bytes() above.
inline std::vector<uint8_t> map_blocks_as_bytes()
{
    std::vector<uint8_t> out(0x100 * 16);
    for (int b = 0; b < 0x100; b++)
        for (int i = 0; i < 16; i++)
            out[b * 16 + i] = (uint8_t)std::clamp(map_blocks[b][i], 0, 255);
    return out;
}

struct PatchResult { bool ok = false; std::string message; fs::path outputPath; };

// Patches a copy of xrickPath with the level currently in map_bnums[],
// writing the result to <name>_patched (same directory, same
// permissions). The original file is never modified.
inline PatchResult patchXrickBinary(const fs::path &xrickPath)
{
    PatchResult res;

    std::ifstream in(xrickPath, std::ios::binary);
    if (!in) { res.message = "Could not open " + xrickPath.string(); return res; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (buf.empty()) { res.message = "File is empty or unreadable"; return res; }

    size_t fileOffset = 0, symSize = 0;
    std::string err;
    if (!find_symbol_file_offset(buf, "map_bnums", fileOffset, symSize, err))
    {
        res.message = "Could not locate the level data in this binary: " + err;
        return res;
    }
    if (symSize != (size_t)MAP_COUNT)
    {
        res.message = "This binary's map_bnums has a different size ("
                     + std::to_string(symSize) + " bytes, expected " + std::to_string(MAP_COUNT)
                     + ") -- likely an incompatible xrick build. Nothing was patched.";
        return res;
    }
    if (fileOffset + MAP_COUNT > buf.size())
    {
        res.message = "map_bnums location falls outside the file -- refusing to patch.";
        return res;
    }

    int outOfRange = 0;
    for (int i = 0; i < MAP_COUNT; i++)
    {
        int v = map_bnums[i];
        if (v < 0 || v > 255) { outOfRange++; v = std::clamp(v, 0, 255); }
        buf[fileOffset + i] = (uint8_t)v;
    }

    fs::path outPath = xrickPath;
    std::string stem = xrickPath.stem().string();
    std::string ext = xrickPath.extension().string();
    outPath.replace_filename(stem + "_patched" + ext);

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) { res.message = "Could not create " + outPath.string(); return res; }
    out.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)buf.size());
    out.close();

    std::error_code ec;
    fs::permissions(outPath,
        fs::status(xrickPath, ec).permissions(),
        fs::perm_options::replace, ec);

    res.ok = true;
    res.outputPath = outPath;
    res.message = "Patched level written to " + outPath.string()
                + (outOfRange > 0 ? " (warning: " + std::to_string(outOfRange) + " block value(s) were out of the 0-255 range and got clamped)" : "");
    return res;
}
