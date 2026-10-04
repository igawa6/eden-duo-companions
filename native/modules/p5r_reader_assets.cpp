// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Romfs-backed assets and game text.
//   - ArtLibrary: the asset-free package's images ("module:p5r:<id>" recipes of p5r_art.rec
//     replayed over the game's romfs), the game's FONT0.FNT, and the prewarmed images.
//   - Reader::LoadText (worker thread) / EnsureText / TakeText: the game-text tables;
//     StartPrewarm builds the manifest's recipes ahead at the lowest scheduling priority.
//   - LoadImage / DecodeFont: the load_image and decode_font extension entry points.
// Threads: ArtLibrary is shared by the host's asset worker and the prewarm thread (lock,
// warm_lock); the text job runs on its own std::async thread and is taken over by the tick
// thread.

#include "p5r_reader.h"

namespace p5r_module {

bool ArtLibrary::Romfs(const EdenDsmodHostApi& host) { // caller holds `lock`
    if (romfs.IsOpen())
        return true;
    if (romfs_tried)
        return false;
    romfs_tried = true;
    if (!romfs.Open(host)) {
        Log(host, EDEN_DSMOD_LOG_ERROR, "DSMod P5R art: romfs unavailable: " + romfs.Error());
        return false;
    }
    Log(host, EDEN_DSMOD_LOG_INFO, "DSMod P5R art: " + romfs.Describe());
    return true;
}
bool ArtLibrary::Ready(const EdenDsmodHostApi& host) {
    if (book_ok)
        return true;
    if (book_tried || !host.read_romfs)
        return false;
    book_tried = true;
    // dualscreen/p5r_art.rec (1.0 layout; the Android installer only accepts .so files under
    // modules/), else modules/p5r_art.rec (0.9.x asset-free test packages)
    const char* Table = "file:p5r_art.rec";
    size_t size = host.read_romfs(host.userdata, Table, 0, nullptr, 0);
    if (size == 0) {
        Table = "file:modules/p5r_art.rec";
        size = host.read_romfs(host.userdata, Table, 0, nullptr, 0);
    }
    std::string text(size, '\0');
    if (size == 0 || size > (size_t(16) << 20) ||
        host.read_romfs(host.userdata, Table, 0, text.data(), size) != size ||
        !book.Parse(std::move(text))) {
        Log(host, EDEN_DSMOD_LOG_ERROR, "DSMod P5R art: cannot read p5r_art.rec");
        return false;
    }
    if (!Romfs(host))
        return false;
    sources = std::make_unique<p5r_recipes::RomfsSources>(romfs);
    engine = std::make_unique<p5r_recipes::Engine>(book, *sources);
    book_ok = true;
    Log(host, EDEN_DSMOD_LOG_INFO, "DSMod P5R art: " + std::to_string(book.size()) + " recipes");
    return true;
}
bool ArtLibrary::Build(const EdenDsmodHostApi& host, std::string_view id, p5r_recipes::Image& out) {
    std::scoped_lock guard{lock};
    if (!Ready(host))
        return false;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = engine->Build(id, out, &err);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    build_ms += ms;
    ok ? ++built : ++failed;
    char line[160];
    if (ok)
        std::snprintf(line, sizeof(line),
                      "DSMod P5R art %.*s %ux%u %.1f ms (%u built, %.0f ms total)", int(id.size()),
                      id.data(), out.w, out.h, ms, built, build_ms);
    else
        std::snprintf(line, sizeof(line), "DSMod P5R art %.*s failed: %.100s", int(id.size()),
                      id.data(), err.c_str());
    // every build at DEBUG (timing studies: --filter Core:Debug); slow ones, every 100th and
    // failures at INFO / WARNING
    Log(host,
        !ok                                ? EDEN_DSMOD_LOG_WARNING
        : (ms >= 20.0 || built % 100 == 0) ? EDEN_DSMOD_LOG_INFO
                                           : EDEN_DSMOD_LOG_DEBUG,
        line);
    return ok;
}
bool ArtLibrary::Font(const EdenDsmodHostApi& host, std::vector<uint8_t>& fnt) {
    std::scoped_lock guard{lock};
    return Romfs(host) && romfs.Read("EN/FONT/FONT0.FNT", fnt);
}
bool ArtLibrary::Get(const EdenDsmodHostApi& host, std::string_view id, p5r_recipes::Image& out) {
    {
        std::scoped_lock guard{warm_lock};
        const std::string key{id};
        requested.insert(key);
        if (const auto it = warm.find(key); it != warm.end()) {
            out = std::move(it->second);
            warm_bytes -= out.rgba.size();
            warm.erase(it);
            ++warm_hits;
            return true;
        }
    }
    return Build(host, id, out);
}
bool ArtLibrary::Warm(const EdenDsmodHostApi& host, const std::string& id) {
    std::scoped_lock guard{lock};
    {
        std::scoped_lock w{warm_lock};
        if (warm_bytes >= WarmBudget)
            return false;
        if (requested.count(id) || warm.count(id))
            return true;
    }
    if (!Ready(host))
        return false;
    p5r_recipes::Image image;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    if (!engine->Build(id, image, &err))
        return true; // the host's own request reports the failure
    build_ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ++built;
    std::scoped_lock w{warm_lock};
    if (!requested.count(id)) {
        warm_bytes += image.rgba.size();
        warm.emplace(id, std::move(image));
    }
    return true;
}
std::unique_ptr<Reader::TextJob> Reader::LoadText(ArtLibrary& art, EdenDsmodHostApi api) {
    auto job = std::make_unique<TextJob>();
    const auto t0 = std::chrono::steady_clock::now();
    bool open = false;
    {
        std::scoped_lock guard{art.lock};
        open = api.read_romfs && art.Romfs(api);
    }
    if (open)
        p5r_text::Load(art.romfs, job->text);
    else
        job->text.error = "romfs unavailable";
    job->ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return job;
}
void Reader::EnsureText() {
    if (!text_tried) {
        text_tried = true;
        if (!text_async) {
            TakeText(LoadText(art, host));
            return;
        }
        try {
            text_job = std::async(std::launch::async, &Reader::LoadText, std::ref(art), host);
            return;
        } catch (...) {
            // no thread available: load on this thread as before
            TakeText(LoadText(art, host));
            return;
        }
    }
    if (text_job.valid() && text_job.wait_for(std::chrono::seconds{0}) == std::future_status::ready)
        TakeText(text_job.get());
}
void Reader::StartPrewarm() {
    if (prewarm_ids.empty() || prewarm_thread.joinable())
        return;
    try {
        prewarm_thread = std::thread([this, api = host] {
#if defined(__linux__)
            setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 19);
#endif
            const auto t0 = std::chrono::steady_clock::now();
            size_t done = 0;
            for (const auto& id : prewarm_ids) {
                if (prewarm_stop || !art.Warm(api, id))
                    break;
                ++done;
            }
            if (api.log) {
                size_t bytes;
                {
                    std::scoped_lock w{art.warm_lock};
                    bytes = art.warm_bytes;
                }
                char line[200];
                std::snprintf(
                    line, sizeof(line),
                    "DSMod P5R art prewarm: %zu/%zu recipes in %.0f ms, %.1f MiB held", done,
                    prewarm_ids.size(),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count(),
                    bytes / 1048576.0);
                api.log(api.userdata, EDEN_DSMOD_LOG_INFO, line);
            }
        });
    } catch (...) {
    }
}
void Reader::TakeText(std::unique_ptr<Reader::TextJob> job) {
    if (!job)
        return;
    StartPrewarm();
    text = std::move(job->text);
    const double ms = job->ms;
    if (host.log) {
        char line[200];
        if (text.ready)
            std::snprintf(line, sizeof(line),
                          "DSMod P5R text: romfs tables ready (%zu music titles, %zu member names, "
                          "arcana 1=%s, stat 0=%s) in %.1f ms (worker thread)",
                          text.music.size(), text.members.size(), text.Arcana(1), text.Stat(0), ms);
        else
            std::snprintf(line, sizeof(line),
                          "DSMod P5R text: tables unavailable (%.120s); names stay empty",
                          text.error.c_str());
        host.log(host.userdata, text.ready ? EDEN_DSMOD_LOG_INFO : EDEN_DSMOD_LOG_ERROR, line);
    }
}
// The game's own lettering: EN/FONT/FONT0.FNT from the user's romfs (a trimmed copy in the same
// format is also accepted); decode_font turns it into metrics and load_image turns the same
// file into the atlas. Both are pure functions of the file, so no state is shared between the
// host's font call and its asset worker.
EdenDsmodBool LoadImage(void* p, const EdenDsmodHostApi* host, const char* key, void* receiver,
                        EdenDsmodImageSink sink) {
    try {
        if (!host || !key || !sink || !host->read_romfs)
            return false;
        const std::string_view k{key};
        auto* reader = static_cast<Reader*>(p);
        // "module:p5r:<id>": a recipe of the package's p5r_art.rec (asset-free art)
        if (reader && k.starts_with("module:p5r:")) {
            p5r_recipes::Image image;
            if (!reader->art.Get(*host, k.substr(11), image) || image.w == 0 || image.h == 0 ||
                image.w > 4096 || image.h > 4096)
                return false;
            sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
            return true;
        }
        // "module:p5r_dec:...": decoder-level debug sources (p5r_romfs_assets.h)
        if (reader && p5r_assets::IsDecoderKey(k)) {
            p5r_assets::Image image;
            {
                std::scoped_lock guard{reader->art.lock};
                if (!p5r_assets::LoadDecoderImage(reader->art.romfs, *host, k, image))
                    return false;
            }
            sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
            return true;
        }
        std::vector<uint8_t> bytes;
        const std::string path{k == "module:p5r_font:romfs:EN/FONT/FONT0.FNT"
                                   ? std::string{"romfs"}
                                   : std::string{p5r_font::AtlasKeyPath(key)}};
        if (path.empty())
            return false;
        if (path == "romfs") {
            // asset-free package: the game's own font out of the CPKs
            if (!reader || !reader->art.Font(*host, bytes))
                return false;
        } else {
            const size_t size = host->read_romfs(host->userdata, path.c_str(), 0, nullptr, 0);
            if (size == 0 || size > 8 * 1024 * 1024)
                return false;
            bytes.resize(size);
            if (host->read_romfs(host->userdata, path.c_str(), 0, bytes.data(), size) != size)
                return false;
        }
        p5r_font::Atlas atlas;
        if (!p5r_font::DecodeAtlas(bytes.data(), bytes.size(), atlas))
            return false;
        sink(receiver, atlas.width, atlas.height, atlas.rgba.data(), atlas.rgba.size());
        return true;
    } catch (...) {
        return false;
    }
}
EdenDsmodBool DecodeFont(void* p, const uint8_t* bytes, size_t size, void* receiver,
                         EdenDsmodFontSink sink) {
    try {
        // Asset-free package: `font` names the recipe table (any package file works; the host only
        // hands its bytes over). Seeing its magic, decode the game's own FONT0.FNT from romfs.
        std::vector<uint8_t> romfs_font;
        if (p && bytes && size >= 6 && std::memcmp(bytes, "P5RREC", 6) == 0) {
            auto* reader = static_cast<Reader*>(p);
            if (!reader->art.Font(reader->Host(), romfs_font))
                return false;
            bytes = romfs_font.data();
            size = romfs_font.size();
        }
        p5r_font::Atlas atlas;
        if (!sink || !p5r_font::DecodeAtlas(bytes, size, atlas))
            return false;
        std::vector<EdenDsmodFontGlyph> glyphs(atlas.glyphs.size());
        for (size_t i = 0; i < glyphs.size(); ++i) {
            const auto& g = atlas.glyphs[i];
            glyphs[i] = {g.x, g.y, g.w, g.h, g.bearing_x, g.bearing_y, g.advance};
        }
        sink(receiver, atlas.line_height, p5r_font::FirstCodepoint, glyphs.data(),
             static_cast<uint32_t>(glyphs.size()));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace p5r_module
