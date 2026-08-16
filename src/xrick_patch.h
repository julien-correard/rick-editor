// xrick_patch.h -- patches a compiled xrick ELF32 executable in place,
// injecting the level currently open in the editor (map_bnums) at its
// original location, found dynamically via the ELF symbol table (works
// for any unstripped xrick build, not just one fixed binary/offset).
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

// Locates a global symbol by name in an ELF32 image already loaded in
// memory. Returns true and fills fileOffset/size on success.
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
        err = "Only 32-bit ELF binaries are supported (this looks like a different format)";
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

    // Find .symtab (+ its linked .strtab) among the sections.
    const Elf32_Shdr *symtabSh = nullptr;
    for (int i = 0; i < eh->e_shnum; i++)
    {
        const Elf32_Shdr *sh = shAt(i);
        if (sh && sh->sh_type == SHT_SYMTAB) { symtabSh = sh; break; }
    }
    if (!symtabSh)
    {
        err = "No symbol table in this binary (it looks stripped) -- "
              "can't locate map_bnums by name";
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
        if (sym.st_shndx == SHN_UNDEF) continue; // undefined (extern) symbol, not the real definition

        const Elf32_Shdr *containingSh = shAt(sym.st_shndx);
        if (!containingSh) { err = "Symbol found but its section is invalid"; return false; }
        if (containingSh->sh_type == SHT_NOBITS) { err = "Symbol is in .bss (no data stored in file)"; return false; }

        fileOffset = containingSh->sh_offset + (sym.st_value - containingSh->sh_addr);
        symSize = sym.st_size;
        return true;
    }

    err = std::string("Symbol '") + symbolName + "' not found in this binary";
    return false;
}

// Generic reusable helper: locate `symbolName` in buf, verify its size
// matches newSize exactly, and overwrite it with newData. Used by
// patchXrickBinary() below and by xrick_levels.h for map_submaps /
// map_connect.
inline bool elf32_patch_symbol(std::vector<uint8_t> &buf, const char *symbolName,
                                const void *newData, size_t newSize, std::string &err)
{
    size_t off = 0, size = 0;
    if (!elf32_find_symbol_file_offset(buf, symbolName, off, size, err)) return false;
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
    if (!elf32_find_symbol_file_offset(buf, "map_bnums", fileOffset, symSize, err))
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
