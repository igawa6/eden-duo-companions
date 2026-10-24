// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only: a minimal EdenDsmodHostApi backed by a local romfs dump directory, for driving
// SmoRomfsTables::Build() (and anything else that only calls host.read_romfs) from host-side
// tests. Never built into the shipped module. Serves "base:/<path>" (the only prefix
// smo_romfs_tables.cpp reads) from "<base_dir>/<path>"; anything else (a "#member" sub-path, a
// different prefix) is a deliberate miss (0), matching the real host's "unknown reads as
// missing" contract closely enough for this module, which always fetches whole archives itself.
#pragma once

#include "core/mods/dsmod_module_abi.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

namespace SmoTestHost {

class FakeHost {
public:
    explicit FakeHost(std::string base_dir) : base_dir_(std::move(base_dir)) {}

    EdenDsmodHostApi Api() {
        EdenDsmodHostApi api{};
        api.userdata = this;
        api.read_romfs = &FakeHost::ReadRomfs;
        return api;
    }

private:
    std::string base_dir_;

    static size_t ReadRomfs(void* userdata, const char* path, uint64_t offset, void* output, size_t size) {
        auto& self = *static_cast<FakeHost*>(userdata);
        const std::string p = path ? path : "";
        constexpr std::string_view prefix = "base:/";
        if (p.size() <= prefix.size() || p.compare(0, prefix.size(), prefix) != 0) return 0;
        const std::string rel = p.substr(prefix.size());
        if (rel.find('#') != std::string::npos) return 0; // this fake never decodes containers itself
        std::ifstream f(self.base_dir_ + "/" + rel, std::ios::binary);
        if (!f) return 0;
        f.seekg(0, std::ios::end);
        const auto file_size = static_cast<uint64_t>(f.tellg());
        if (f.fail() || offset > file_size) return 0;
        const uint64_t avail = file_size - offset;
        if (!output) return avail > SIZE_MAX ? SIZE_MAX : size_t(avail);
        const size_t n = size_t(std::min<uint64_t>(avail, uint64_t(size)));
        f.seekg(std::streamoff(offset));
        f.read(static_cast<char*>(output), std::streamsize(n));
        return f.good() || f.eof() ? n : 0;
    }
};

} // namespace SmoTestHost
