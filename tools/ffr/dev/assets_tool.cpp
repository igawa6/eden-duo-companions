// SPDX-FileCopyrightText: Copyright 2026 Daniel Vergara
// SPDX-License-Identifier: GPL-3.0-or-later
// Desktop check of ffr_assets against an extracted RomFS folder (development only).
// usage: assets_tool <romfs dir> <out dir>
#include <chrono>
#include <cstdio>
#include <string>
#include "../../../native/modules/ffr_assets.h"
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::string root = argv[1], out = argv[2];
    ffr::RomRead rd = [&](const char* p, uint64_t off, void* dst, size_t n) -> size_t {
        std::string path = p; if (path.rfind("romfs:/", 0) == 0) path = path.substr(7);
        FILE* f = std::fopen((root + "/" + path).c_str(), "rb"); if (!f) return 0;
        std::fseek(f, 0, SEEK_END); size_t sz = size_t(std::ftell(f));
        if (!dst) { std::fclose(f); return sz; }
        std::fseek(f, long(off), SEEK_SET); size_t got = std::fread(dst, 1, n, f); std::fclose(f); return got; };
    auto t0 = std::chrono::steady_clock::now();
    auto ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    ffr::IoStore io; ffr::Pak pak; std::string err;
    if (!io.Open(rd, "romfs:/FFRS/Content/Paks/FFRS-Switch.utoc", "romfs:/FFRS/Content/Paks/FFRS-Switch.ucas", err)) { std::printf("io: %s\n", err.c_str()); return 1; }
    std::printf("io %zu files %.0f ms\n", io.FileCount(), ms());
    if (!pak.Open(rd, "romfs:/FFRS/Content/Paks/FFRS-Switch.pak", err)) { std::printf("pak: %s\n", err.c_str()); return 1; }
    std::printf("pak %zu files %.0f ms\n", pak.FileCount(), ms());
    std::vector<uint8_t> b; std::unordered_map<std::string, std::string> loc;
    if (!pak.Read("FFRS/Content/Localization/Game/en/Game.locres", b) || !ffr::ParseLocres(b, loc)) { std::printf("locres failed\n"); return 1; }
    std::printf("locres %zu strings %.0f ms: %s / %s\n", loc.size(), ms(), loc["ST_UnitName\x1fUnitName1001"].c_str(), loc["ST_EquipmentName\x1f" "EquipmentName10100"].c_str());
    std::vector<uint8_t> ttf;
    if (!pak.Read("FFRS/Content/UI/Font/FontFace/SE/SE-VOGEN.ufont", b) || !ffr::UnwrapFontFace(b, ttf)) { std::printf("font failed\n"); return 1; }
    std::printf("font %zu bytes %.4s\n", ttf.size(), (const char*)ttf.data() );
    for (const char* t : {"FFRS/Content/UI/Textures/Icon/Menu/ICON_Item_Katana.uasset", "FFRS/Content/UI/Textures/Picture/Char/T_Chara_Button_1001.uasset",
                          "FFRS/Content/UI/Textures/MapCapture/Town_01Gra_22/Town_01Gra_22_6.uasset", "FFRS/Content/UI/Textures/Menu/T_Window_Frame_Box_4_2.uasset"}) {
        ffr::Image im;
        if (!io.Read(t, b) || !ffr::DecodeTexture(b, im)) { std::printf("tex failed %s\n", t); continue; }
        std::string name = std::string(t); name = name.substr(name.rfind('/') + 1);
        FILE* f = std::fopen((out + "/" + name + ".rgba").c_str(), "wb"); std::fwrite(im.rgba.data(), 1, im.rgba.size(), f); std::fclose(f);
        std::printf("tex %s %dx%d %.0f ms\n", name.c_str(), im.w, im.h, ms());
    }
}
