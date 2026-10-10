// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Host-side check of the Wonder catalog, run against a romfs dump:
//   wonder-catalog-tool <romfs> dump [lang]             every catalog row (TSV, see below)
//   wonder-catalog-tool <romfs> route <resource>         one area's route
//   wonder-catalog-tool <romfs> routes [lang]            routes of every catalog area
//   wonder-catalog-tool <romfs> course-routes <id>       linked per-area rails of one course
//   wonder-catalog-tool <romfs> byml <path>              a BYML/.bgyml/.byml.zs file as JSON
//   wonder-catalog-tool <romfs> msbt <sarc.zs> <member>  an MSBT inside a (zstd) SARC
// Rows: C course name | W world key course | R course area resource | A course label resource
//       N world name title internal. It exercises exactly the code the module ships.

#include <cmath>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "wonder_catalog.h"

using namespace WonderCatalog;

static std::optional<std::vector<std::uint8_t>> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
}

static std::string Json(std::string_view s) {
    std::string o = "\"";
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            o += '\\';
            o += ch;
        } else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        } else {
            o += ch;
        }
    }
    return o + "\"";
}

static void PrintNode(const BymlNode& n, int indent) {
    const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
    using T = BymlNode::Type;
    switch (n.type) {
    case T::Null:
        std::printf("null");
        break;
    case T::String:
        std::printf("%s", Json(n.str).c_str());
        break;
    case T::Binary:
        std::printf("\"<binary %zu bytes>\"", n.str.size());
        break;
    case T::Bool:
        std::printf(n.num.b ? "true" : "false");
        break;
    case T::Int:
    case T::Int64:
        std::printf("%lld", static_cast<long long>(n.num.i));
        break;
    case T::UInt:
    case T::UInt64:
        std::printf("%llu", static_cast<unsigned long long>(n.num.u));
        break;
    case T::Float:
    case T::Double:
        std::printf("%.9g", n.num.f);
        break;
    case T::Array:
        std::printf("[");
        for (std::size_t i = 0; i < n.array.size(); ++i) {
            std::printf("%s\n%s  ", i ? "," : "", pad.c_str());
            PrintNode(n.array[i], indent + 1);
        }
        std::printf("%s]", n.array.empty() ? "" : ("\n" + pad).c_str());
        break;
    case T::Dict:
        std::printf("{");
        for (std::size_t i = 0; i < n.dict.size(); ++i) {
            std::printf("%s\n%s  %s: ", i ? "," : "", pad.c_str(), Json(n.dict[i].key).c_str());
            PrintNode(n.dict[i].value, indent + 1);
        }
        std::printf("%s}", n.dict.empty() ? "" : ("\n" + pad).c_str());
        break;
    }
}

static void PrintRoute(const std::string& res, const Route& r) {
    auto pt = [](const std::optional<RoutePoint>& p) {
        char b[64];
        if (p)
            std::snprintf(b, sizeof(b), "%.2f,%.2f", p->x, p->y);
        else
            std::snprintf(b, sizeof(b), "-");
        return std::string(b);
    };
    std::printf("ROUTE\t%s\tstart=%s\tnormal=%s\tsecret=%s\tlength=%.2f\tmarkers=%zu\tgoal_id=%d\n",
                res.c_str(), pt(r.start).c_str(), pt(r.normal_goal).c_str(),
                pt(r.secret_goal).c_str(), r.length, r.markers.size(), r.normal_goal_id);
    for (const auto& m : r.markers)
        std::printf("M\t%s\t%d\t%d\t%.2f\t%.2f\t%.3f\n", res.c_str(), m.kind, m.id, m.x, m.y,
                    m.progress);
}

static std::string Clean(std::string s) {
    for (auto& ch : s)
        if (ch == '\t' || ch == '\n' || ch == '\r')
            ch = ' ';
    return s;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <romfs> dump [lang] | route <res> | routes [lang] | course-routes <id> | byml <path> | "
                     "msbt <sarc.zs> <member>\n",
                     argv[0]);
        return 2;
    }
    const std::string root = argv[1], op = argv[2];
    const WonderAssets::RomfsReader read = [&](const std::string& p) { return ReadFile(root + p); };
    if (op == "course-routes" && argc == 4) {
        int course = -1;
        const std::string arg = argv[3];
        const auto result = std::from_chars(arg.data(), arg.data() + arg.size(), course);
        if (result.ec != std::errc{} || result.ptr != arg.data() + arg.size())
            return 2;
        const auto routes = BuildCourseRoutes(read, course);
        for (const auto& [res, route] : routes)
            PrintRoute(res, route);
        return routes.empty() ? 1 : 0;
    }
    if (op == "dump" || op == "routes") {
        const std::string lang = argc >= 4 ? argv[3] : "USen";
        const auto cat = BuildCatalog(read, lang, /*details=*/true);
        if (!cat) {
            std::fprintf(stderr, "catalog build failed\n");
            return 1;
        }
        if (op == "routes") {
            int ok = 0, fail = 0;
            for (const auto& [key, res] : cat->area_resource) {
                if (const auto r = BuildRoute(read, res)) {
                    PrintRoute(res, *r);
                    ++ok;
                } else {
                    std::printf("NOROUTE\t%s\n", res.c_str());
                    ++fail;
                }
            }
            std::fprintf(stderr, "routes: %d built, %d without start/goal\n", ok, fail);
            return 0;
        }
        for (const auto& [w, name] : cat->world_name) {
            const auto t = cat->world_title.find(w);
            const auto i = cat->world_internal.find(w);
            std::printf("N\t%d\t%s\t%s\t%s\n", w, Clean(name).c_str(),
                        t != cat->world_title.end() ? Clean(t->second).c_str() : "",
                        i != cat->world_internal.end() ? i->second.c_str() : "");
        }
        for (const auto& [id, name] : cat->course_name)
            std::printf("C\t%d\t%s\n", id, Clean(name).c_str());
        for (const auto& [k, id] : cat->world_course)
            std::printf("W\t%d\t%d\t%d\n", k.first, k.second, id);
        for (const auto& [k, res] : cat->area_resource)
            std::printf("R\t%d\t%d\t%s\n", k.first, k.second, res.c_str());
        for (const auto& [k, label] : cat->area_label) {
            const auto res = cat->area_resource.find(k);
            std::printf("A\t%d\t%s\t%s\n", k.first, Clean(label).c_str(),
                        res != cat->area_resource.end() ? res->second.c_str() : "");
        }
        return 0;
    }
    if (op == "route" && argc >= 4) {
        const auto r = BuildRoute(read, argv[3]);
        if (!r) {
            std::fprintf(stderr, "no route for %s\n", argv[3]);
            return 1;
        }
        PrintRoute(argv[3], *r);
        return 0;
    }
    if (op == "byml" && argc >= 4) {
        const auto n = LoadByml(read, argv[3]);
        if (!n) {
            std::fprintf(stderr, "parse failed: %s\n", argv[3]);
            return 1;
        }
        PrintNode(*n, 0);
        std::printf("\n");
        return 0;
    }
    if (op == "msbt" && argc >= 5) {
        const auto raw = read(argv[3]);
        const auto sarc = raw ? WonderAssets::Zstd(*raw) : std::nullopt;
        const auto member = sarc ? WonderAssets::SarcMember(*sarc, argv[4]) : std::nullopt;
        const auto msgs = member ? ParseMsbt(*member) : std::nullopt;
        if (!msgs) {
            std::fprintf(stderr, "msbt failed: %s %s\n", argv[3], argv[4]);
            return 1;
        }
        for (const auto& [label, text] : *msgs)
            std::printf("%s\t%s\n", label.c_str(), Clean(text).c_str());
        return 0;
    }
    return 2;
}
