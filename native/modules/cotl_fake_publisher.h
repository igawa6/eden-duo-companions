// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <string>

#include "cotl_publish.h"

struct FakePublisher : cotl::Publisher {
    std::map<std::string, int64_t> i64;
    std::map<std::string, double> f64;
    std::map<std::string, std::string> text;

    void I64(const std::string& n, int64_t v) override { i64[n] = v; }
    void F64(const std::string& n, double v) override { f64[n] = v; }
    void Text(const std::string& n, std::string_view v) override { text[n] = std::string(v); }

    // Number of published names with this prefix (e.g. "fol.name")
    size_t CountPrefix(const std::string& prefix) const {
        size_t n = 0;
        for (const auto* m : {&i64}) {
            for (const auto& [k, v] : *m) n += k.rfind(prefix, 0) == 0;
        }
        for (const auto& [k, v] : f64) n += k.rfind(prefix, 0) == 0;
        for (const auto& [k, v] : text) n += k.rfind(prefix, 0) == 0;
        return n;
    }
};
