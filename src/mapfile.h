// mapfile.h -- format de fichier .map propre a cet editeur.
//
//   octets 0-3   magic "RKMP"
//   octets 4-7   uint32 version (=1)
//   octets 8-11  uint32 count (doit valoir MAP_COUNT, 8152)
//   octets 12..  count * int32, valeurs de map_bnums dans l'ordre
#pragma once

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <filesystem>

#include "mapdata.h"

namespace fs = std::filesystem;

static const int MAP_COLS = 8;
static const int MAP_ROWS = (int)(sizeof(map_bnums) / sizeof(map_bnums[0])) / MAP_COLS; // 1019
static const int MAP_COUNT = MAP_COLS * MAP_ROWS;

struct MapFileHeader { char magic[4]; uint32_t version; uint32_t count; };

inline bool saveMapFile(const fs::path &path, std::string &err)
{
    FILE *f = std::fopen(path.string().c_str(), "wb");
    if (!f) { err = "Could not open file for writing"; return false; }
    MapFileHeader hdr{{'R','K','M','P'}, 1, (uint32_t)MAP_COUNT};
    bool ok = std::fwrite(&hdr, sizeof(hdr), 1, f) == 1
           && std::fwrite(map_bnums, sizeof(int), MAP_COUNT, f) == (size_t)MAP_COUNT;
    std::fclose(f);
    if (!ok) { err = "Write error"; return false; }
    return true;
}

inline bool loadMapFile(const fs::path &path, std::string &err)
{
    FILE *f = std::fopen(path.string().c_str(), "rb");
    if (!f) { err = "Could not open file"; return false; }
    MapFileHeader hdr{};
    if (std::fread(&hdr, sizeof(hdr), 1, f) != 1 || std::memcmp(hdr.magic, "RKMP", 4) != 0)
    {
        std::fclose(f); err = "Not a valid .map file"; return false;
    }
    if (hdr.version != 1 || hdr.count != (uint32_t)MAP_COUNT)
    {
        std::fclose(f); err = "Unsupported or mismatched .map file"; return false;
    }
    bool ok = std::fread(map_bnums, sizeof(int), MAP_COUNT, f) == (size_t)MAP_COUNT;
    std::fclose(f);
    if (!ok) { err = "Read error"; return false; }
    return true;
}
