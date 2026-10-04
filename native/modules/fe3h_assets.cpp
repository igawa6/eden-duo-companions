// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe3h_assets.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <unordered_map>

#include "fe3h_font.h"
#include "fe3h_g1t.h"
#include "fe3h_romfs.h"
#include "fe3h_ui2d.h"

namespace fe3h_assets {
namespace {

using fe3h_romfs::SourcePtr;

// One section of a fixed_* table: ptbl child = 0x40 header {u32 magic, u32 count, u32 stride}
// + count x stride rows.
// One layer of a class icon (global sprite id, offset in the 32 x 32 cell).
struct IconLayer {
    int64_t id;
    int32_t dx, dy;
};

struct Table {
    const uint8_t* rows{};
    uint32_t count{}, stride{};
    const uint8_t* Row(uint32_t i) const {
        return i < count ? rows + size_t(i) * stride : nullptr;
    }
};

bool Section(const std::vector<uint8_t>& file, uint32_t index, uint32_t min_stride, Table& t) {
    if (file.size() < 4)
        return false;
    uint32_t n;
    std::memcpy(&n, file.data(), 4);
    if (index >= n || 4 + uint64_t(n) * 8 > file.size())
        return false;
    uint32_t off, size;
    std::memcpy(&off, file.data() + 4 + size_t(index) * 8, 4);
    std::memcpy(&size, file.data() + 8 + size_t(index) * 8, 4);
    if (uint64_t(off) + size > file.size() || size < 0x40)
        return false;
    const uint8_t* s = file.data() + off;
    std::memcpy(&t.count, s + 4, 4);
    std::memcpy(&t.stride, s + 8, 4);
    if (t.stride < min_stride || t.count > 0x10000 || 0x40 + uint64_t(t.count) * t.stride > size)
        return false;
    t.rows = s + 0x40;
    return true;
}

int16_t I16(const uint8_t* p) {
    int16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
uint16_t U16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

// "123" -> 123 (decimal, no sign, whole string).
bool ParseU32(std::string_view s, uint32_t& v) {
    if (s.empty() || s.size() > 10)
        return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

std::vector<std::string_view> Split(std::string_view s) {
    std::vector<std::string_view> out;
    size_t p = 0;
    while (true) {
        const size_t q = s.find('/', p);
        out.push_back(s.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p));
        if (q == std::string_view::npos)
            return out;
        p = q + 1;
    }
}

// The v5 redesign's composed keys (LibraryImpl::Styled): fe3h:bg/, blur/, win/, patch/, mirror/,
// crop/, num/.
bool IsStyledKey(std::string_view k) {
    for (const std::string_view p : {"fe3h:bg/", "fe3h:blur/", "fe3h:win/", "fe3h:patch/",
                                     "fe3h:mirror/", "fe3h:crop/", "fe3h:num/", "fe3h:scrolltrack/"})
        if (k.starts_with(p))
            return true;
    return false;
}

// Faces at or above this id live in the Cindered Shadows face file (0x7B22), not in 6123
// (0x5EAFCC: face - 0x413 <= 0x72 -> loader slot 0x12).
constexpr int32_t FirstDlcFace = 0x413;
constexpr int32_t LastDlcFace = 0x413 + 0x72;  // 0x5EAFD4: face - 0x413 <= 0x72 -> slot 0x12
constexpr uint32_t FaceDlcEntry = 0x7B22;      // 31522 (INFO1 362): still/face_school_normal_DLC.bin
constexpr uint32_t DlcFaceFlag = 0x80000000u;  // internal: index names a child of FaceDlcEntry
constexpr int32_t FaceIdLimit = 0x486; // 0x6A1030: >= -> the row's column 0
constexpr uint32_t MaxPerson = 0x4B0;  // 0x3E69A8 / 0x6A1088
constexpr uint32_t StillEntry = 0x17EE;    // 6126: face_btl_l_cmn, 450 KT-gz G1Ts 256^2
constexpr uint32_t StillDlcEntry = 0x7B23; // 31523 (INFO1 363): still/face_btl_l_cmn_DLC.bin
constexpr uint32_t ClassDataEntry = 11;  // fixed_classdata, s0 100 x 0x76
constexpr uint32_t FixedDataEntry = 6;   // fixed_data, s0 weapons 500 x 0x18 (item id - 10)

// A GOT slot that should point at main + table. Before the game's own loader has applied the
// relocations (the first frames after boot, when a package's first images are requested) the
// slot still holds its unrelocated value (0 / the bare offset): the table itself is plain
// rodata and already readable then, so only a slot holding something ELSE is a mismatch.
bool GotSlotOk(uint64_t slot, uint64_t base, uint64_t table) {
    return slot == base + table || slot == table || slot == 0;
}

// C remainder (sign follows the dividend), as the game's sdiv / csel sequences compute it.
int32_t CMod(int32_t a, int32_t m) {
    return a % m;
}

// 0x3E79A0: whether a face row takes the per-unit variant offset. Rows 0x10D..0x255 go through
// the jump table at main+0xCE8B84; these are the ones whose target returns 0 (read from the
// 1.2.0 main with research/fe3h tools; every other row in that range returns 1).
bool RowTakesVariant(int32_t row) {
    if (row < 0x64 || (row >= 0x226 && row < 0x233))
        return false;
    static constexpr uint16_t NoVariant[] = {
        269, 270, 271, 272, 317, 362, 363, 364, 365, 426, 455, 456, 457, 458, 525, 526, 527, 528,
        529, 530, 531, 532, 533, 534, 535, 536, 537, 538, 539, 540, 541, 542, 543, 544, 545, 546,
        547, 548, 549, 550, 551, 552, 553, 554, 555, 556, 557, 558, 559, 560, 561, 562, 595, 596,
        597};
    if (row >= 0x10D && row <= 0x255) {
        for (const uint16_t r : NoVariant)
            if (r == row)
                return false;
        return true;
    }
    return row != 0x7C;
}

// 0x46230 with the arguments 0x3E6980 passes (w6 = w7 = -1, [sp+8] = 0): the variant code of a
// unit (0x46..0x73), or a value >= 0x74 / < 0 for none. `rec` = persondata s1[asset].
int32_t VariantCode(int32_t asset, const uint8_t* rec, int32_t slot, uint32_t pid, uint32_t gender,
                    bool part2, int32_t var) {
    if (var >= 0 && var < 0x74)
        return var;
    if (asset > 0x257 || !rec)
        return -1;
    int32_t w8;
    if (part2) {
        w8 = I16(rec + 6);
        if (!(w8 >= 0 && w8 <= 0x73))
            w8 = I16(rec + 2);
    } else {
        w8 = I16(rec + 2);
    }
    if (pid <= MaxPerson && !(w8 >= 0 && w8 < 0x74))
        w8 = var;
    const bool small = w8 >= 0 && w8 < 0x74;
    if (rec[0x10] != 0 || (asset >= 0x64 && asset < 0xB4)) {
        if (small)
            return w8;
        if (gender == 1)
            return 0x4E + CMod(slot, 4);
        if (gender != 0)
            return 0x46;
        return 0x46 + CMod(slot, 8);
    }
    if (asset >= 0x122 && asset <= 0x132) {
        if (small)
            return w8;
        if (gender == 1)
            return 0x4E + CMod(slot, asset == 0x123 ? 3 : 4);
        if (gender != 0)
            return 0x46;
        return 0x46 + CMod(slot, 4);
    }
    if (asset == 0x13A || asset == 0x133 || asset == 0x134) {
        if (small)
            return w8;
        if (gender == 1)
            return 0x5F;
        if (gender != 0)
            return 0x5E;
        return asset - 0x133 > 1 ? 0x61 : 0x5E;
    }
    if (asset == 0x135 || asset == 0x136) {
        if (small)
            return w8;
        if (gender == 1)
            return 0x58 + CMod(slot, 3);
        if (gender != 0)
            return 0x52;
        return 0x52 + CMod(slot, 3);
    }
    return w8;
}

// 0x3E6F24..0x3E6F98: variant code -> row offset.
int32_t VariantOffset(int32_t code) {
    if (code >= 0x46 && code <= 0x4D)
        return code - 0x46;
    if (code >= 0x4E && code <= 0x51)
        return code - 0x4E;
    if (code >= 0x52 && code <= 0x54)
        return code - 0x52;
    if (code >= 0x58 && code <= 0x5A)
        return code - 0x58;
    return 0;
}

using Image = fe3h_g1t::Image;

} // namespace

class LibraryImpl {
public:
    explicit LibraryImpl(size_t budget) : budget_(budget) {}

    bool Load(const EdenDsmodHostApi* host, std::string_view key, std::vector<uint8_t>& rgba,
              uint32_t& w, uint32_t& h);
    bool Face(const EdenDsmodHostApi* host, uint32_t pid, int32_t expr, bool part2,
              uint32_t& index);
    bool UnitRow(const EdenDsmodHostApi* host, const UnitFaceInput& in, uint32_t& row);
    bool UnitFace(const EdenDsmodHostApi* host, const UnitFaceInput& in, int32_t expr,
                  uint32_t& index);
    int WeaponIcon(uint32_t item, uint32_t uses) const; // any thread, never blocks
    bool DecodeFont(const EdenDsmodHostApi* host, std::span<const uint8_t> bytes, FontOut& out);

private:
    struct Pixels {
        uint8_t format{};
        uint32_t w{}, h{};
        std::vector<uint8_t> data;
    };

    // All below: io_ held.
    bool Ready(const EdenDsmodHostApi* host);
    SourcePtr Container(uint32_t id, SourcePtr& slot);
    bool Child(SourcePtr& container_slot, uint32_t id, uint32_t index, SourcePtr& g1t);
    bool Persons();
    bool FaceFromRow(uint32_t row, int32_t expr, uint32_t& index);
    bool FaceFromPerson(uint32_t pid, int32_t expr, bool part2, uint32_t& index);
    bool FaceChild(uint32_t index, SourcePtr& g1t); // 6123 child, or DlcFaceFlag | 0x7B22 child
    bool Classes();
    bool Weapons();
    bool UnitRowLocked(const UnitFaceInput& in, uint32_t& row);
    bool UiRects(const EdenDsmodHostApi* host);
    std::optional<Pixels> Fetch(std::string_view key);
    // Cropped kinds: a decoded parent image (cached) and a rect inside it.
    bool Crop(const EdenDsmodHostApi* host, std::string_view key, std::string& parent,
              uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h);
    static std::optional<Pixels> Tex(fe3h_romfs::Source& g1t, uint32_t index);

    std::shared_ptr<const Image> CacheGet(const std::string& key);
    void CachePut(const std::string& key, std::shared_ptr<const Image> img);

    std::mutex io_;
    std::unique_ptr<fe3h_romfs::LinkData> link_;
    bool link_failed_{};
    SourcePtr portraits_, faces_, minimaps_, abbey_;
    bool persons_tried_{};
    bool persons_ok_{};
    std::vector<uint8_t> persondata_;
    Table s0_, s1_, s16_, p2s3_;
    bool classes_tried_{}, classes_ok_{};
    std::vector<uint8_t> classdata_;
    Table cls_;
    bool weapons_tried_{}, weapons_ok_{};
    std::vector<uint8_t> fixeddata_;
    Table weapons_;
    // set (release) once fixeddata_ / weapons_ are loaded; they never change afterwards, so a
    // reader that saw it true reads them without io_
    std::atomic<bool> weapons_ready_{false};
    struct UiRect {
        uint16_t tex, x, y, w, h, tw, th;
    };
    bool ui_tried_{}, ui_ok_{};
    std::vector<UiRect> ui_;
    // Global sprite ids (fe3h:sprite/...): slots 3 and 4 of the atlas manager (slot 0 = ui_).
    bool slot_tried_[16]{}, slot_ok_[16]{};
    std::vector<UiRect> slot_rects_[16];
    bool SlotRects(const EdenDsmodHostApi* host, int k);
    SourcePtr ui_atlas_[UiAtlasLast - UiAtlasFirst + 1];
    std::map<uint32_t, SourcePtr> ui_g1t_;
    SourcePtr font_tex_, stills_, stills_dlc_, faces_dlc_;
    uint32_t arc_id_{};
    std::vector<uint8_t> arc_; // the last ui2d SARC read (they are read whole, <= 18 MiB)
    std::vector<fe3h_ui2d::SarcFile> arc_files_;
    bool Ui2d(const EdenDsmodHostApi* host, uint32_t arc, std::string_view name,
              fe3h_g1t::Image& out);
    bool Compose(const EdenDsmodHostApi* host, std::string_view key, fe3h_g1t::Image& out);
    bool Panel(const EdenDsmodHostApi* host, std::string_view key, fe3h_g1t::Image& out);
    bool Nine(const EdenDsmodHostApi* host, std::string_view key, fe3h_g1t::Image& out);
    bool Portrait(const EdenDsmodHostApi* host, std::string_view key, fe3h_g1t::Image& out);
    bool Gauge(const EdenDsmodHostApi* host, std::string_view key, fe3h_g1t::Image& out);
    bool GambitRange(const EdenDsmodHostApi* host, std::string_view key, fe3h_g1t::Image& out);
    bool ClassIcon(const EdenDsmodHostApi* host, std::string_view key, fe3h_g1t::Image& out);
    // the v5 redesign's composed keys (bg / blur / win / patch / mirror / crop / num)
    bool Styled(const EdenDsmodHostApi* host, std::string_view key, fe3h_g1t::Image& out);
    bool Inner(const EdenDsmodHostApi* host, std::string_view key, fe3h_g1t::Image& out);
    int64_t IconClassSprite(uint32_t cls, uint32_t gender);
    bool ClassIconLayers(uint32_t cls, uint32_t gender, uint32_t face, bool p2, uint32_t army,
                         uint32_t pid, bool route_alt, std::vector<IconLayer>& out);
    // A sprite's whole atlas texture + its rect (fe3h:sprite resolution); false if unresolved.
    bool SpriteSource(const EdenDsmodHostApi* host, uint32_t id, fe3h_g1t::Image& tex,
                      uint32_t rect[4]);

    std::mutex cache_mu_;
    size_t budget_;
    size_t used_{};
    using CacheList = std::list<std::pair<std::string, std::shared_ptr<const Image>>>;
    CacheList lru_;
    std::unordered_map<std::string, CacheList::iterator> cache_index_;
    std::set<std::string> failed_;
};

bool LibraryImpl::Ready(const EdenDsmodHostApi* host) {
    if (link_)
        return link_->IsOpen();
    if (link_failed_ || !host || !host->read_romfs)
        return false;
    link_ = std::make_unique<fe3h_romfs::LinkData>(fe3h_romfs::HostReader(*host));
    if (!link_->Open()) {
        link_.reset();
        link_failed_ = true;
        return false;
    }
    return true;
}

SourcePtr LibraryImpl::Container(uint32_t id, SourcePtr& slot) {
    if (!slot)
        slot = link_->Entry(id);
    return slot;
}

// A KT-gz G1T child of a ptbl entry (6122, 6123), inflated whole (children are <= ~1 MiB).
bool LibraryImpl::Child(SourcePtr& container_slot, uint32_t id, uint32_t index, SourcePtr& g1t) {
    const SourcePtr c = Container(id, container_slot);
    const SourcePtr child = c ? fe3h_romfs::PtblChild(c, index) : nullptr;
    std::vector<uint8_t> packed, plain;
    if (!child || child->Size() > (uint64_t{16} << 20) || !child->ReadAll(packed) ||
        !fe3h_romfs::KtGzDecompress(packed, plain, uint64_t{32} << 20))
        return false;
    g1t = fe3h_romfs::Memory(std::move(plain));
    return true;
}

bool LibraryImpl::Persons() {
    if (persons_tried_)
        return persons_ok_;
    persons_tried_ = true;
    const SourcePtr e = link_->Entry(PersonDataEntry);
    if (!e || !e->ReadAll(persondata_, uint64_t{8} << 20))
        return false;
    // min strides cover every field read: s0 up to +0x26 (gender), s1 up to +0x10 (VariantCode),
    // s16 up to +0x16, s3 up to +0x21
    persons_ok_ = Section(persondata_, 0, 0x27, s0_) && Section(persondata_, 1, 0x11, s1_) &&
                  Section(persondata_, 16, 0x17, s16_) && Section(persondata_, 3, 0x22, p2s3_);
    return persons_ok_;
}

bool LibraryImpl::FaceFromRow(uint32_t row, int32_t expr, uint32_t& index) {
    if (!Persons())
        return false;
    const uint8_t* r = s16_.Row(row);
    if (!r)
        return false;
    uint32_t slot = FaceSlot(expr);
    if (slot != 0 && r[0x16] == 0)
        slot = 0;
    int32_t face = I16(r + slot * 2);
    if (face < 0 || face >= FaceIdLimit)
        face = I16(r); // 0x6A1088: the row's column 0
    if (face < 0)
        return false;
    if (face >= FirstDlcFace) {
        // the Cindered Shadows / DLC face file (0x5EAF40 slot 0x12, child face - 0x413)
        if (face > LastDlcFace)
            return false;
        uint32_t count{};
        const SourcePtr d = Container(FaceDlcEntry, faces_dlc_);
        if (!d || !fe3h_romfs::PtblCount(d, count) || uint32_t(face - FirstDlcFace) >= count)
            return false;
        index = DlcFaceFlag | uint32_t(face - FirstDlcFace);
        return true;
    }
    uint32_t count{};
    const SourcePtr c = Container(FaceEntry, faces_);
    if (!c || !fe3h_romfs::PtblCount(c, count) || uint32_t(face) >= count)
        return false;
    index = uint32_t(face);
    return true;
}

bool LibraryImpl::FaceChild(uint32_t index, SourcePtr& g1t) {
    if (index & DlcFaceFlag)
        return Child(faces_dlc_, FaceDlcEntry, index & ~DlcFaceFlag, g1t);
    return Child(faces_, FaceEntry, index, g1t);
}

bool LibraryImpl::FaceFromPerson(uint32_t pid, int32_t expr, bool part2, uint32_t& index) {
    if (!Persons() || pid > MaxPerson)
        return false;
    const uint8_t* p = s0_.Row(pid);
    if (!p)
        return false;
    const uint8_t* a = s1_.Row(U16(p + 0x18));
    if (!a)
        return false;
    int32_t row = I16(a + (part2 ? 4 : 8));
    if (row < 0)
        row = I16(a + 8); // 0x3E6B78: the Part I column
    if (row < 0)
        return false;
    return FaceFromRow(uint32_t(row), expr, index);
}

std::optional<LibraryImpl::Pixels> LibraryImpl::Tex(fe3h_romfs::Source& g1t, uint32_t index) {
    const auto t = fe3h_g1t::Texture(g1t, index);
    if (!t)
        return std::nullopt;
    Pixels px{t->format, t->width, t->height, {}};
    if (!g1t.ReadVec(t->data_offset, static_cast<size_t>(t->data_size), px.data))
        return std::nullopt;
    return px;
}

std::optional<LibraryImpl::Pixels> LibraryImpl::Fetch(std::string_view key) {
    if (key.starts_with("module:"))
        key.remove_prefix(7);
    if (!key.starts_with("fe3h:"))
        return std::nullopt;
    key.remove_prefix(5);
    const auto part = Split(key);
    std::vector<uint32_t> n;
    for (size_t i = 1; i < part.size() && part[0] != "font"; ++i) {
        uint32_t v;
        if (!ParseU32(part[i], v))
            return std::nullopt;
        n.push_back(v);
    }
    const std::string_view kind = part[0];
    SourcePtr g1t;
    uint32_t tex = 0;
    if (kind == "portrait" && n.size() == 1) {
        g1t = Container(PortraitEntry, portraits_);
        tex = n[0];
    } else if (kind == "face" && (n.size() == 2 || n.size() == 3)) {
        if (n.size() == 3 && n[2] != 1 && n[2] != 2)
            return std::nullopt;
        uint32_t idx;
        if (!FaceFromPerson(n[0], int32_t(n[1]), n.size() == 3 && n[2] == 2, idx) ||
            !FaceChild(idx, g1t))
            return std::nullopt;
    } else if (kind == "facerow" && n.size() == 2) {
        uint32_t idx;
        if (!FaceFromRow(n[0], int32_t(n[1]), idx) || !FaceChild(idx, g1t))
            return std::nullopt;
    } else if (kind == "faceidx" && n.size() == 1) {
        if (!Child(faces_, FaceEntry, n[0], g1t))
            return std::nullopt;
    } else if (kind == "bmap" && (n.size() == 1 || n.size() == 2)) {
        if (n.size() == 2 && n[1] > 1)
            return std::nullopt;
        const int idx = MinimapIndex(n[0], n.size() == 2 && n[1] == 1);
        if (idx < 0 || !Child(minimaps_, MinimapEntry, uint32_t(idx), g1t))
            return std::nullopt;
    } else if (kind == "abbey" && n.size() == 1) {
        g1t = Container(AbbeyEntry, abbey_);
        tex = n[0];
    } else if (kind == "unitface" && n.size() >= 4 && n.size() <= 6) {
        if (n.size() == 6 && n[5] != 1 && n[5] != 2)
            return std::nullopt;
        UnitFaceInput in;
        in.pid = n[0];
        in.cls = n[1];
        in.gender_byte = n[2];
        in.slot = n[3] > 0x68 ? -1 : int32_t(n[3]);
        in.part2 = n.size() == 6 && n[5] == 2;
        uint32_t row, idx;
        if (!UnitRowLocked(in, row) || !FaceFromRow(row, n.size() >= 5 ? int32_t(n[4]) : 0, idx) ||
            !FaceChild(idx, g1t))
            return std::nullopt;
    } else if (kind == "font" && part.size() == 2 && part[1] == "latin") {
        g1t = Container(fe3h_font::TextureEntry, font_tex_);
    } else if (kind == "uitex" && n.size() == 2) {
        if ((n[0] < UiRangeFirst || n[0] > UiRangeLast) && n[0] != 31503) // + 700_patch (g9)
            return std::nullopt;
        g1t = Container(n[0], ui_g1t_[n[0]]);
        tex = n[1];
    } else if (kind == "still" && n.size() >= 4 && n.size() <= 5) {
        if (n.size() == 5 && n[4] != 1 && n[4] != 2)
            return std::nullopt;
        UnitFaceInput in;
        in.pid = n[0];
        in.cls = n[1];
        in.gender_byte = n[2];
        in.slot = n[3] > 0x68 ? -1 : int32_t(n[3]);
        in.part2 = n.size() == 5 && n[4] == 2;
        uint32_t row;
        if (!UnitRowLocked(in, row))
            return std::nullopt;
        if (row < 0x64) { // 0x5D9830: atlas id 0xF6F3 + row -> the roster sheet, texture = row
            g1t = Container(PortraitEntry, portraits_);
            tex = row;
        } else if (row >= 0x226 && row < 0x233) {
            return std::nullopt; // 0x796C + row: served through fe3h:sprite in Load
        } else if (row <= 0x225) { // 0x6A1660 type 4 -> LINKDATA 0x17EE, child row - 0x64
            if (!Child(stills_, StillEntry, row - 0x64, g1t))
                return std::nullopt;
        } else if (row <= 0x255) { // type 0x15 -> LINKDATA 0x7B23 (INFO1), child row - 0x233
            if (!Child(stills_dlc_, StillDlcEntry, row - 0x233, g1t))
                return std::nullopt;
        } else {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    if (!g1t)
        return std::nullopt;
    return Tex(*g1t, tex);
}

std::shared_ptr<const Image> LibraryImpl::CacheGet(const std::string& key) {
    std::scoped_lock lock{cache_mu_};
    if (const auto found = cache_index_.find(key); found != cache_index_.end()) {
        lru_.splice(lru_.begin(), lru_, found->second);
        return found->second->second;
    }
    return nullptr;
}

void LibraryImpl::CachePut(const std::string& key, std::shared_ptr<const Image> img) {
    std::scoped_lock lock{cache_mu_};
    const size_t size = img->rgba.size();
    if (size > budget_)
        return;
    // Concurrent image requests can both miss before decoding. Keep one entry per key so
    // those requests cannot consume the budget twice or evict unrelated textures.
    if (const auto found = cache_index_.find(key); found != cache_index_.end()) {
        lru_.splice(lru_.begin(), lru_, found->second);
        return;
    }
    lru_.emplace_front(key, std::move(img));
    cache_index_.emplace(key, lru_.begin());
    used_ += size;
    while (used_ > budget_ && !lru_.empty()) {
        used_ -= lru_.back().second->rgba.size();
        cache_index_.erase(lru_.back().first);
        lru_.pop_back();
    }
}

bool LibraryImpl::Load(const EdenDsmodHostApi* host, std::string_view key_view,
                   std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
    if (!weapons_ready_.load(std::memory_order_acquire)) {
        // the weapon rows the reader's item icons need (WeaponIcon), loaded here on the worker
        std::scoped_lock lock{io_};
        if (!weapons_tried_ && Ready(host))
            Weapons();
    }
    const std::string key{key_view.starts_with("module:") ? key_view.substr(7) : key_view};
    if (key.starts_with("fe3h:ui2d/") || key.starts_with("fe3h:dport") ||
        key.starts_with("fe3h:panel") || key.starts_with("fe3h:sport") ||
        key.starts_with("fe3h:oport/") || key.starts_with("fe3h:gauge/") ||
        key.starts_with("fe3h:adjport/") ||
        key.starts_with("fe3h:titlewin/") || key.starts_with("fe3h:paperwin/") ||
        key.starts_with("fe3h:nine/") || key.starts_with("fe3h:gbrange/") ||
        key.starts_with("fe3h:clsicon/") || IsStyledKey(key)) {
        if (auto hit = CacheGet(key)) {
            rgba = hit->rgba;
            w = hit->width;
            h = hit->height;
            return true;
        }
        fe3h_g1t::Image img;
        bool ok;
        if (key.starts_with("fe3h:ui2d/")) {
            const auto part = Split(std::string_view{key}.substr(10));
            uint32_t arc;
            ok = part.size() == 2 && ParseU32(part[0], arc) && !part[1].empty();
            if (ok) {
                std::scoped_lock lock{io_};
                ok = !failed_.contains(key) && Ready(host) && Ui2d(host, arc, part[1], img);
                if (!ok && failed_.size() < 4096)
                    failed_.insert(key);
            }
        } else if (key.starts_with("fe3h:panel") || key.starts_with("fe3h:titlewin/") ||
                   key.starts_with("fe3h:paperwin/")) {
            ok = Panel(host, key, img);
        } else if (key.starts_with("fe3h:nine/")) {
            ok = Nine(host, key, img);
        } else if (key.starts_with("fe3h:gbrange/")) {
            ok = GambitRange(host, key, img);
        } else if (key.starts_with("fe3h:clsicon/")) {
            ok = ClassIcon(host, key, img);
        } else if (key.starts_with("fe3h:sport") || key.starts_with("fe3h:oport/") ||
                   key.starts_with("fe3h:adjport/")) {
            ok = Portrait(host, key, img);
        } else if (key.starts_with("fe3h:gauge/")) {
            ok = Gauge(host, key, img);
        } else if (IsStyledKey(key)) {
            ok = Styled(host, key, img);
        } else {
            ok = Compose(host, key, img);
        }
        if (!ok)
            return false;
        rgba = img.rgba;
        w = img.width;
        h = img.height;
        CachePut(key, std::make_shared<const Image>(std::move(img)));
        return true;
    }
    if (key.starts_with("fe3h:ui/") || key.starts_with("fe3h:fontglyph/") ||
        key.starts_with("fe3h:g1tcrop/") || key.starts_with("fe3h:sprite/")) {
        if (auto hit = CacheGet(key)) {
            rgba = hit->rgba;
            w = hit->width;
            h = hit->height;
            return true;
        }
        std::string parent;
        uint32_t cx, cy, cw, ch;
        {
            std::scoped_lock lock{io_};
            if (failed_.contains(key) || !Ready(host))
                return false;
            if (!Crop(host, key, parent, cx, cy, cw, ch)) {
                // Not latched: a sprite / ui crop can fail only while the guest tables are not
                // readable yet, or for a bad id / texture (cheap to re-check).
                return false;
            }
        }
        std::vector<uint8_t> full;
        uint32_t fw, fh;
        if (!Load(host, parent, full, fw, fh) || uint64_t(cx) + cw > fw || uint64_t(cy) + ch > fh)
            return false;
        rgba.resize(size_t(cw) * ch * 4);
        for (uint32_t row = 0; row < ch; ++row)
            std::memcpy(rgba.data() + size_t(row) * cw * 4,
                        full.data() + (size_t(cy + row) * fw + cx) * 4, size_t(cw) * 4);
        w = cw;
        h = ch;
        CachePut(key, std::make_shared<const Image>(Image{cw, ch, rgba}));
        return true;
    }
    if (key.starts_with("fe3h:sil/")) {
        // fe3h:sil/<RRGGBB>/<key>: any other fe3h image as a flat silhouette in that colour (the
        // source alpha kept, the colour replaced), e.g. fe3h:sil/C8352E/sprite/718 (loading screen).
        const std::string_view rest = std::string_view{key}.substr(9);
        const auto slash = rest.find('/');
        if (slash != 6)
            return false;
        uint32_t rgb = 0;
        for (const char c : rest.substr(0, 6)) {
            const int d = c >= '0' && c <= '9' ? c - '0'
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0)
                return false;
            rgb = rgb << 4 | uint32_t(d);
        }
        if (!Load(host, "fe3h:" + std::string{rest.substr(7)}, rgba, w, h))
            return false;
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            rgba[i] = uint8_t(rgb >> 16);
            rgba[i + 1] = uint8_t(rgb >> 8);
            rgba[i + 2] = uint8_t(rgb);
        }
        return true;
    }
    if (key.starts_with("fe3h:still/")) {
        // Named DLC rows 0x226..0x232 (Cindered Shadows students, Anna, Jeritza ...): 0x5D5AC0
        // shows atlas sprite 0x796C + row (group 9 = LINKDATA 31503, textures 5..17, 256^2).
        const auto part = Split(std::string_view{key}.substr(11));
        std::vector<uint32_t> n;
        for (const auto p : part) {
            uint32_t v;
            if (!ParseU32(p, v))
                return false;
            n.push_back(v);
        }
        if ((n.size() == 4 || n.size() == 5) && (n.size() == 4 || n[4] == 1 || n[4] == 2)) {
            UnitFaceInput in;
            in.pid = n[0];
            in.cls = n[1];
            in.gender_byte = n[2];
            in.slot = n[3] > 0x68 ? -1 : int32_t(n[3]);
            in.part2 = n.size() == 5 && n[4] == 2;
            uint32_t row = 0;
            bool ok;
            {
                std::scoped_lock lock{io_};
                ok = Ready(host) && UnitRowLocked(in, row);
            }
            if (ok && row >= 0x226 && row < 0x233)
                return Load(host, "fe3h:sprite/" + std::to_string(0x796C + row), rgba, w, h);
        }
    }
    if (auto hit = CacheGet(key)) {
        rgba = hit->rgba;
        w = hit->width;
        h = hit->height;
        return true;
    }
    std::optional<Pixels> px;
    {
        std::scoped_lock lock{io_};
        if (failed_.contains(key) || !Ready(host))
            return false;
        px = Fetch(key);
        if (!px) {
            if (failed_.size() < 4096)
                failed_.insert(key);
            return false;
        }
    }
    // Block decoding runs outside the romfs lock (text lookups from other threads stay cheap).
    auto img = fe3h_g1t::DecodePixels(px->format, px->w, px->h, px->data);
    if (!img)
        return false;
    rgba = img->rgba;
    w = img->width;
    h = img->height;
    CachePut(key, std::make_shared<const Image>(std::move(*img)));
    return true;
}

bool LibraryImpl::Face(const EdenDsmodHostApi* host, uint32_t pid, int32_t expr, bool part2,
                   uint32_t& index) {
    std::scoped_lock lock{io_};
    return Ready(host) && FaceFromPerson(pid, expr, part2, index) && !(index & DlcFaceFlag);
}

bool LibraryImpl::Classes() {
    if (classes_tried_)
        return classes_ok_;
    classes_tried_ = true;
    const SourcePtr e = link_->Entry(ClassDataEntry);
    classes_ok_ = e && e->ReadAll(classdata_, uint64_t{8} << 20) &&
                  Section(classdata_, 0, 0x0C, cls_) && cls_.count > 0;
    return classes_ok_;
}

bool LibraryImpl::Weapons() {
    if (weapons_tried_)
        return weapons_ok_;
    weapons_tried_ = true;
    const SourcePtr e = link_->Entry(FixedDataEntry);
    weapons_ok_ = e && e->ReadAll(fixeddata_, uint64_t{8} << 20) &&
                  Section(fixeddata_, 0, 0x17, weapons_) && weapons_.count > 0;
    weapons_ready_.store(weapons_ok_, std::memory_order_release);
    return weapons_ok_;
}

// 0x3E6980 for a unit on the battle map (the slot >= 0 path). Row = persondata s1[asset] +8
// (Part I) / +4 (Part II, < 0 -> +8); generic rows (>= 0x64, not 0x226..0x232) on the battle map
// take the class's asset (classdata s0 +8 + gender * 2, used when <= 0x257; gender = unit+0x104
// when <= 2, else persondata +0x26; gender 2 -> no override) and a variant offset (0x46230,
// applied when 0x3E79A0 accepts the row). Named special cases: pid 0x9B / 0x19D -> row 0x224 +
// (gender == 1); pid 0x224 / 0x225 -> that row; row 0x24 on battle map 0x24 -> 0x39. Not modelled
// (they need save state): pid 3 in Part II on route 3, the DLC persons 0x412 / 0x416, and the
// slot < 0 path (0x3E6BA8, the unit under the cursor / acting unit).
bool LibraryImpl::UnitRowLocked(const UnitFaceInput& in, uint32_t& row_out) {
    if (!Persons() || !Classes() || in.pid > MaxPerson)
        return false;
    const uint8_t* p = s0_.Row(in.pid);
    if (!p)
        p = s0_.Row(0); // out of range -> entry 0, as the table manager does
    if (!p)
        return false;
    int32_t asset = U16(p + 0x18);
    const int32_t var = I16(p + 0x10);
    uint32_t gender = p[0x26];
    auto s1row = [&](int32_t a) {
        const uint8_t* r = a >= 0 ? s1_.Row(uint32_t(a)) : nullptr;
        return r ? r : s1_.Row(0);
    };
    auto face_row = [&](int32_t a) {
        const uint8_t* r = s1row(a);
        if (!r)
            return int32_t(-1);
        int32_t v = I16(r + (in.part2 ? 4 : 8));
        if (v < 0)
            v = I16(r + 8);
        return v;
    };
    const int32_t row0 = face_row(asset);
    int32_t offset = 0;
    const bool generic = !(row0 < 0x64 || (row0 >= 0x226 && row0 < 0x233));
    if (in.slot >= 0 && in.in_battle && generic) {
        if (in.gender_byte <= 2)
            gender = in.gender_byte;
        if (gender != 2) {
            const uint8_t* c = cls_.Row(in.cls);
            if (!c)
                c = cls_.Row(0);
            // persondata's gender byte is not bounded: a column past the row is no override
            if (c && 8 + size_t(gender) * 2 + 2 <= cls_.stride) {
                const int32_t o = I16(c + 8 + gender * 2);
                if (uint16_t(o) <= 0x257)
                    asset = o;
            }
            offset = VariantOffset(
                VariantCode(asset, s1row(asset), in.slot, in.pid, gender, in.part2, var));
        }
    }
    int32_t row = face_row(asset);
    if (in.pid == 0x9B || in.pid == 0x19D)
        row = 0x224 + (gender == 1 ? 1 : 0);
    else if (in.pid == 0x224 || in.pid == 0x225)
        row = int32_t(in.pid);
    if (row == 0x24 && in.in_battle && in.battle_map == 0x24)
        row = 0x39;
    if (row < 0)
        return false;
    if (RowTakesVariant(row))
        row += offset;
    if (!s16_.Row(uint32_t(row)))
        return false;
    row_out = uint32_t(row);
    return true;
}

bool LibraryImpl::UnitRow(const EdenDsmodHostApi* host, const UnitFaceInput& in, uint32_t& row) {
    std::scoped_lock lock{io_};
    return Ready(host) && UnitRowLocked(in, row);
}

bool LibraryImpl::UnitFace(const EdenDsmodHostApi* host, const UnitFaceInput& in, int32_t expr,
                           uint32_t& index) {
    std::scoped_lock lock{io_};
    uint32_t row;
    return Ready(host) && UnitRowLocked(in, row) && FaceFromRow(row, expr, index) &&
           !(index & DlcFaceFlag);
}

// 0x5D2A20 (weapon ids 10..509; row = fixed_data s0[id - 10]): +4 effect, +5 weapon type,
// +9 crest (0x56 none; 0x2C / 0x2D special), +0xD item type, +0xF / +0x16 flag bytes (bit 7).
// `uses` = the slot's uses byte (0 = broken: the game's test 0x40CAB0 when its w1 is 0).
// Called on the tick thread (the reader): never takes io_ and never reads romfs. The table is
// loaded by the asset worker (Load); until then WeaponIconPending.
int LibraryImpl::WeaponIcon(uint32_t item, uint32_t uses) const {
    if (item < 10 || item > 509)
        return -1;
    if (!weapons_ready_.load(std::memory_order_acquire))
        return WeaponIconPending;
    const uint8_t* r = weapons_.Row(item - 10);
    if (!r)
        return -1;
    const uint32_t effect = r[4], wt = r[5], crest = r[9], it = r[0xD];
    if (r[0xF] & 0x80)
        return 0x204;
    const bool relic = crest != 0x2C && crest != 0x2D && crest < 0x56;
    const bool f03 = (r[0x16] & 0x80) != 0;
    if (wt <= 4) {
        if (uses == 0)
            return int(0x21D + wt);
        if (f03)
            return int(0x215 + wt);
        if (crest == 0x2C)
            return int(0x205 + wt);
        return int(relic ? 0x20D + wt : 0x1FC + wt);
    }
    switch (wt) {
    case 5:
        return 0x202;
    case 6:
        return 0x201;
    case 7:
        return 0x203;
    case 8:
        if (it > 3)
            return 0x1D4;
        if (f03)
            return int(0x1EA + it);
        if (crest == 0x2C)
            return int(0x1E2 + it);
        return int(relic ? 0x1E6 + it : 0x1D4 + it);
    case 9:
    case 11:
        if (effect == 0x17 && crest != 0x56)
            return int(crest + 0x7A);
        return int(0x1D4 + it);
    case 10:
        return 0x1D7;
    default:
        return 0x1FC;
    }
}

// The common UI atlas rect table from guest memory, pinned by its getter and GOT slot.
bool LibraryImpl::UiRects(const EdenDsmodHostApi* host) {
    if (ui_tried_)
        return ui_ok_;
    if (!host || !host->read_memory || !host->main_base)
        return false; // not latched: a later call with a full host may succeed
    auto read = [&](uint64_t off, void* out, size_t n) {
        return off + n <= host->main_size &&
               (!host->is_mapped || host->is_mapped(host->userdata, host->main_base + off, n)) &&
               host->read_memory(host->userdata, host->main_base + off, out, n);
    };
    uint32_t words[4]{};
    uint64_t slot{};
    std::vector<uint8_t> raw(size_t(1024) * 16);
    if (!read(UiRectGetter, words, sizeof(words)) || !read(UiRectGot, &slot, 8) ||
        !read(UiRectTable, raw.data(), raw.size()))
        return false; // not readable yet: not latched, the next request retries
    ui_tried_ = true;
    if (!GotSlotOk(slot, host->main_base, UiRectTable))
        return false;
    for (int i = 0; i < 4; ++i)
        if (words[i] != UiRectGetterWords[i])
            return false;
    for (uint32_t k = 0; k < 1024; ++k) {
        uint16_t f[8];
        std::memcpy(f, raw.data() + size_t(k) * 16, 16);
        if (f[1] != k) // the id column counts up; the next table restarts at 0
            break;
        if (f[0] > 32 || f[4] == 0 || f[5] == 0 || uint32_t(f[2]) + f[4] > f[6] ||
            uint32_t(f[3]) + f[5] > f[7])
            return false;
        ui_.push_back({f[0], f[2], f[3], f[4], f[5], f[6], f[7]});
    }
    ui_ok_ = ui_.size() > 700;
    return ui_ok_;
}

namespace {
// Atlas-manager id ranges (0x5C35A0 / 0x5C3790: global id -> slot object, local id = id - base)
// and the rect tables their slot objects read through the getters 0x6A84E0 / 0x6A8600 /
// 0x6A8660 (GOT 0x1AB0B78 / 0x1AB0B90 / 0x1AB0B98). The per-language G1T of a slot is
// base LINKDATA + language (ENG_U = 1), matched by texture count and every texture size.
struct SpriteSlot {
    uint32_t first, span, atlas;
    bool per_language; // texture file = atlas + language, else the fixed LINKDATA atlas
    uint32_t max_records;
    uint64_t table, got, getter;
    uint32_t got_ldr;     // the getter's "ldr x8, [x8, #slot]" word
    bool indexed = true;  // record i carries i in its second u16 (group 11 does not: gaps)
};
// Group g of the atlas manager [[GOT 0x1AA8748]] + 8 * g; rect table = [GOT 0x1AB0B78 + 8 * g]
// (getter 0x6A84E0 + 0x60 * g: adrp x8 / ldr x8, [x8, #slot] / add x0, x8, w1, sxtw #4 / ret).
// Texture files: group 0 / 3 / 4 = per-language atlases (base + language); 5 / 6 / 7 / 9 = one
// file (loader 0x69B880 / 0x69B6C0, research/fe3h/re4 §0).
constexpr SpriteSlot SpriteSlots[] = {
    {0x0000, 0x2D6, 6026, true, 725, 0xD52D64, 0x1AB0B78, 0x6A84E0, 0xF945BD08},  // g0 common UI
    {0x05B8, 0x127, 6050, true, 295, 0xD588D4, 0x1AB0B90, 0x6A8600, 0xF945C908},  // g3 battle UI
    {0x06DF, 0x87F, 6062, true, 416, 0xD5F6C4, 0x1AB0B98, 0x6A8660, 0xF945CD08},  // g4 monastery UI
    {0x0F5E, 0xF6B, 6074, false, 13, 0xD67EB4, 0x1AB0BA0, 0x6A86C0, 0xF945D108},  // g5 abbey map
    {0x1EC9, 0x1EDE, 6086, false, 21, 0xD77564, 0x1AB0BA8, 0x6A8720, 0xF945D508}, // g6 conversation
    {0x3DA7, 0x3DAD, 6097, false, 6, 0xD96344, 0x1AB0BB0, 0x6A8780, 0xF945D908},  // g7 headers / vines
    {0x7B54, 0x7B9F, 31503, false, 75, 0x11991C4, 0x1AB0BC0, 0x6A8840, 0xF945E108}, // g9 Abyss UI
    {0xF6F3, 0xF7EA, 6108, false, 247, 0x1199674, 0x1AB0BC8, 0x6A88A0, 0xF945E508}, // g10 roster
    // g11 map-unit sprites (class icons, 0x5CF7E0 -> consumer 0x6A2240; loader 0x69B880 w0 =
    // 0x17DD): the dispatch accepts local <= 0x1F4A1, the table holds 0x5C5 real records (tex 0
    // 1024^2 32 x 32 whole sprites / bodies, tex 1 / 2 512^2 20 x 20 heads + mounts, tex 3 128^2);
    // unused locals are placeholder copies of record 0 with n = 0.
    {0x1EEDD, 0x5C5, 6109, false, 0x5C5, 0x1291514, 0x1AB0BD0, 0x6A8900, 0xF945E908, false},
};
constexpr size_t SpriteSlotCount = sizeof(SpriteSlots) / sizeof(SpriteSlots[0]);
// 0x5C35A0 .. +0x180: the id-range dispatch these groups rely on (0x5C35A0 / 0x5C3790 twins).
constexpr uint32_t SpriteDispatch = 0x5C35A0;
constexpr uint32_t SpriteDispatchWords[96] = {
    0x710B543F, 0x540001C9, 0x510B5828, 0x710B751F, 0x540000C8, 0x91002000, 0x2A0803E1, 0xF9400000,
    0xB5000120, 0x1400006E, 0x121E7428, 0x7116D11F, 0x54000101, 0x5116D021, 0x91004000, 0xF9400000,
    0xB4000CE0, 0xF9400008, 0xF9400102, 0xD61F0040, 0x5116E028, 0x7104991F, 0x540000C8, 0x91006000,
    0x2A0803E1, 0xF9400000, 0xB5FFFEE0, 0x1400005C, 0x511B7C28, 0x7121F91F, 0x540000C8, 0x91008000,
    0x2A0803E1, 0xF9400000, 0xB5FFFDE0, 0x14000054, 0x513D7828, 0x713DA91F, 0x540000C8, 0x9100A000,
    0x2A0803E1, 0xF9400000, 0xB5FFFCE0, 0x1400004C, 0x1283D908, 0x0B080028, 0x53017D09, 0x713DB93F,
    0x540000C8, 0x9100C000, 0x2A0803E1, 0xF9400000, 0xB5FFFBA0, 0x14000042, 0x1287B4C8, 0x0B080028,
    0x5287B589, 0x6B09011F, 0x540000C8, 0x9100E000, 0x2A0803E1, 0xF9400000, 0xB5FFFA60, 0x14000038,
    0x128F6A68, 0x0B080028, 0x528F73C9, 0x6B09011F, 0x540000C8, 0x91012000, 0x2A0803E1, 0xF9400000,
    0xB5FFF920, 0x1400002E, 0x129EDE48, 0x0B080028, 0x529EFD29, 0x6B09011F, 0x540000C8,
    0x91014000, 0x2A0803E1, 0xF9400000, 0xB5FFF7E0, 0x14000024, 0x52822468, 0x72BFFFC8,
    0x0B080028, 0x529E9429, 0x72A00029, 0x6B09011F, 0x540000C8, 0x91016000, 0x2A0803E1,
    0xF9400000, 0xB5FFF660, 0x14000018};
} // namespace

// Rect table of SpriteSlots[k] (k >= 1; k = 0 is ui_), read from guest memory.
bool LibraryImpl::SlotRects(const EdenDsmodHostApi* host, int k) {
    if (slot_tried_[k])
        return slot_ok_[k];
    if (!host || !host->read_memory || !host->main_base)
        return false;
    const SpriteSlot& sl = SpriteSlots[k];
    auto read = [&](uint64_t off, void* out, size_t n) {
        return off + n <= host->main_size &&
               (!host->is_mapped || host->is_mapped(host->userdata, host->main_base + off, n)) &&
               host->read_memory(host->userdata, host->main_base + off, out, n);
    };
    uint32_t words[4]{}, disp[96]{};
    uint64_t slot{};
    std::vector<uint8_t> raw(size_t(sl.max_records) * 16);
    if (!read(sl.getter, words, sizeof(words)) || !read(sl.got, &slot, 8) ||
        !read(SpriteDispatch, disp, sizeof(disp)) || !read(sl.table, raw.data(), raw.size()))
        return false; // not readable yet: not latched, the next request retries
    slot_tried_[k] = true;
    const uint32_t getter_words[4] = {0x9000A048, sl.got_ldr, 0x8B21D100, 0xD65F03C0};
    if (!GotSlotOk(slot, host->main_base, sl.table) ||
        std::memcmp(words, getter_words, sizeof(words)) != 0 ||
        std::memcmp(disp, SpriteDispatchWords, sizeof(disp)) != 0)
        return false;
    for (uint32_t i = 0; i < sl.max_records; ++i) {
        uint16_t f[8];
        std::memcpy(f, raw.data() + size_t(i) * 16, 16);
        if ((sl.indexed && f[1] != i) || f[0] > 255 || f[4] == 0 || f[5] == 0 || uint32_t(f[2]) + f[4] > f[6] ||
            uint32_t(f[3]) + f[5] > f[7])
            return false;
        slot_rects_[k].push_back({f[0], f[2], f[3], f[4], f[5], f[6], f[7]});
    }
    slot_ok_[k] = true;
    return true;
}

bool LibraryImpl::Crop(const EdenDsmodHostApi* host, std::string_view key, std::string& parent,
                       uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h) {
    key.remove_prefix(5); // "fe3h:"
    const auto part = Split(key);
    std::vector<uint32_t> n;
    for (size_t i = 1; i < part.size(); ++i) {
        uint32_t v;
        if (!ParseU32(part[i], v))
            return false;
        n.push_back(v);
    }
    if (part[0] == "fontglyph" && n.size() == 1) {
        if (!fe3h_font::SerifCell(n[0], x, y))
            return false;
        w = fe3h_font::SerifCellW;
        h = fe3h_font::SerifCellH;
        parent = "fe3h:font/latin";
        return true;
    }
    if (part[0] == "g1tcrop" && n.size() == 6) {
        if (n[0] < UiRangeFirst || n[0] > UiRangeLast)
            return false;
        const SourcePtr c = Container(n[0], ui_g1t_[n[0]]);
        const auto t = c ? fe3h_g1t::Texture(*c, n[1]) : std::nullopt;
        if (!t || n[4] == 0 || n[5] == 0 || uint64_t(n[2]) + n[4] > t->width ||
            uint64_t(n[3]) + n[5] > t->height)
            return false;
        x = n[2];
        y = n[3];
        w = n[4];
        h = n[5];
        parent = "fe3h:uitex/" + std::to_string(n[0]) + "/" + std::to_string(n[1]);
        return true;
    }
    if (part[0] == "sprite" && (n.size() == 1 || n.size() == 2)) {
        // fe3h:sprite/<global id>[/<language 0..11, default 1 = ENG_U>]
        const uint32_t id = n[0], lang = n.size() == 2 ? n[1] : 1;
        if (lang > 11)
            return false;
        int slot = -1;
        for (int k = 0; k < int(SpriteSlotCount); ++k)
            if (id >= SpriteSlots[k].first && id - SpriteSlots[k].first < SpriteSlots[k].span)
                slot = k;
        if (slot < 0)
            return false;
        const uint32_t local = id - SpriteSlots[slot].first;
        const UiRect* r = nullptr;
        if (slot == 0) {
            if (UiRects(host) && local < ui_.size())
                r = &ui_[local];
        } else if (SlotRects(host, slot) && local < slot_rects_[slot].size()) {
            r = &slot_rects_[slot][local];
        }
        if (!r)
            return false;
        const uint32_t entry = SpriteSlots[slot].atlas + (SpriteSlots[slot].per_language ? lang : 0);
        const SourcePtr c = Container(entry, ui_g1t_[entry]);
        const auto t = c ? fe3h_g1t::Texture(*c, r->tex) : std::nullopt;
        if (!t || t->width != r->tw || t->height != r->th)
            return false;
        x = r->x;
        y = r->y;
        w = r->w;
        h = r->h;
        parent = "fe3h:uitex/" + std::to_string(entry) + "/" + std::to_string(r->tex);
        return true;
    }
    if (part[0] == "ui" && (n.size() == 1 || n.size() == 2)) {
        const uint32_t entry = n.size() == 2 ? n[0] : UiAtlasEngU;
        const uint32_t id = n.back();
        if (entry < UiAtlasFirst || entry > UiAtlasLast || !UiRects(host) || id >= ui_.size())
            return false;
        const UiRect& r = ui_[id];
        // The language's atlas texture must be the size the table was made for.
        const SourcePtr c = Container(entry, ui_atlas_[entry - UiAtlasFirst]);
        const auto t = c ? fe3h_g1t::Texture(*c, r.tex) : std::nullopt;
        if (!t || t->width != r.tw || t->height != r.th)
            return false;
        x = r.x;
        y = r.y;
        w = r.w;
        h = r.h;
        parent = "fe3h:uitex/" + std::to_string(entry) + "/" + std::to_string(r.tex);
        return true;
    }
    return false;
}

// A texture of a ui2d archive (io_ held). The archive is read whole and kept until another one
// is asked for; the decoded textures go to the image cache.
bool LibraryImpl::Ui2d(const EdenDsmodHostApi*, uint32_t arc, std::string_view name,
                       fe3h_g1t::Image& out) {
    if ((arc < fe3h_ui2d::FirstArc || arc > fe3h_ui2d::LastArc) && arc != fe3h_ui2d::ExtraArc)
        return false;
    if (arc_id_ != arc) {
        arc_.clear();
        arc_files_.clear();
        arc_id_ = 0;
        const SourcePtr e = link_->Entry(arc);
        if (!e || !e->ReadAll(arc_, uint64_t{32} << 20))
            return false;
        arc_files_ = fe3h_ui2d::Sarc(arc_);
        arc_id_ = arc;
    }
    for (const auto& f : arc_files_) {
        if (!f.name.ends_with(".bntx"))
            continue;
        auto t = fe3h_ui2d::BntxDecode(std::span<const uint8_t>{arc_}.subspan(f.offset, f.size),
                                       name);
        if (!t)
            continue;
        out = {t->width, t->height, std::move(t->rgba)};
        return true;
    }
    return false;
}

namespace {

// Area-average resample of straight RGBA (premultiplied while averaging) to d x d.
fe3h_g1t::Image Resample(const fe3h_g1t::Image& src, uint32_t d) {
    fe3h_g1t::Image out{d, d, std::vector<uint8_t>(size_t(d) * d * 4)};
    const double sx = double(src.width) / d, sy = double(src.height) / d;
    for (uint32_t y = 0; y < d; ++y) {
        const double y0 = y * sy, y1 = (y + 1) * sy;
        for (uint32_t x = 0; x < d; ++x) {
            const double x0 = x * sx, x1 = (x + 1) * sx;
            double acc[4]{}, area = 0;
            for (uint32_t yy = uint32_t(y0); yy < src.height && yy < y1; ++yy) {
                const double wy = std::min<double>(yy + 1, y1) - std::max<double>(yy, y0);
                for (uint32_t xx = uint32_t(x0); xx < src.width && xx < x1; ++xx) {
                    const double wgt =
                        wy * (std::min<double>(xx + 1, x1) - std::max<double>(xx, x0));
                    const uint8_t* p = src.rgba.data() + (size_t(yy) * src.width + xx) * 4;
                    const double a = p[3] / 255.0;
                    for (int c = 0; c < 3; ++c)
                        acc[c] += wgt * p[c] * a;
                    acc[3] += wgt * p[3];
                    area += wgt;
                }
            }
            uint8_t* q = out.rgba.data() + (size_t(y) * d + x) * 4;
            if (area <= 0 || acc[3] <= 0)
                continue;
            const double a = acc[3] / area;
            for (int c = 0; c < 3; ++c)
                q[c] = uint8_t(std::clamp<long>(std::lround(acc[c] / area / (a / 255.0)), 0, 255));
            q[3] = uint8_t(std::lround(a));
        }
    }
    return out;
}

// dst = src over dst (straight alpha), src alpha scaled by mask alpha / 255 (mask may be null).
void Over(fe3h_g1t::Image& dst, const fe3h_g1t::Image& src, const fe3h_g1t::Image* mask) {
    for (size_t i = 0; i < size_t(dst.width) * dst.height; ++i) {
        const uint8_t* s = src.rgba.data() + i * 4;
        uint8_t* d = dst.rgba.data() + i * 4;
        const double sa = s[3] / 255.0 * (mask ? mask->rgba[i * 4 + 3] / 255.0 : 1.0);
        const double da = d[3] / 255.0;
        const double oa = sa + da * (1 - sa);
        if (oa <= 0) {
            d[0] = d[1] = d[2] = d[3] = 0;
            continue;
        }
        for (int c = 0; c < 3; ++c)
            d[c] = uint8_t(std::lround((s[c] * sa + d[c] * da * (1 - sa)) / oa));
        d[3] = uint8_t(std::lround(oa * 255));
    }
}

// LayoutKit material colour: lerp(black, white, texel) * vertex colour.
void Tint(fe3h_g1t::Image& img, const uint8_t black[4], const uint8_t vcol[4]) {
    for (size_t i = 0; i < img.rgba.size(); i += 4)
        for (int c = 0; c < 4; ++c) {
            const int v = black[c] + img.rgba[i + c] * (255 - black[c]) / 255;
            img.rgba[i + c] = uint8_t(v * vcol[c] / 255);
        }
}

} // namespace

// fe3h:dport/<army>/<face key> (208 x 208) and fe3h:dport_s/<army>/<face key> (88 x 88): the
// forecast's portrait diamond (btl_confirm.bflyt, arc 30799), composed as the layout draws it.
//   large  pane p_base_chara_<colour>_l (208^2, untinted) then p_still_p_l (208^2): texture 0 =
//          the unit's still at full UV, texture 1 = cmn_win_still_diamond_mask^t (its alpha
//          clips; open at the top so hair and hats rise out of the diamond).
//   small  p_base_chara_<colour>_mini (cmn_win_still_diamond_s_base^q, material black
//          #000d26 / #170100 and vertex colour #2a89ff / #ff5445 for player / enemy), then
//          frame_chara (s_frame, untinted), then p_still_p_NN (still + cmn_win_still_mini_mask^t).
//          The layout has mini panes for the player and enemy sides only: armies 2 / 3 fail.
// Colours: army 0 player = blue, 1 enemy = red, 2 ally = green, 3 other = yellow.
bool LibraryImpl::Compose(const EdenDsmodHostApi* host, std::string_view key,
                          fe3h_g1t::Image& out) {
    const bool small = key.starts_with("fe3h:dport_s/");
    key.remove_prefix(small ? 13 : 11);
    const size_t slash = key.find('/');
    uint32_t army;
    if (slash == std::string_view::npos || !ParseU32(key.substr(0, slash), army) || army > 3)
        return false;
    const std::string_view face = key.substr(slash + 1);
    static constexpr std::string_view FaceKinds[] = {"portrait/", "face/", "faceidx/",
                                                     "facerow/", "unitface/", "still/"};
    bool allowed = false;
    for (const auto k : FaceKinds)
        allowed = allowed || face.starts_with(k);
    if (!allowed || (small && army > 1))
        return false;
    auto piece = [&](const char* name, fe3h_g1t::Image& img) {
        std::vector<uint8_t> px;
        uint32_t w{}, h{};
        if (!Load(host, "fe3h:ui2d/30799/" + std::string(name), px, w, h))
            return false;
        img = {w, h, std::move(px)};
        return true;
    };
    fe3h_g1t::Image still;
    {
        std::vector<uint8_t> px;
        uint32_t w{}, h{};
        if (!Load(host, "fe3h:" + std::string(face), px, w, h))
            return false;
        still = {w, h, std::move(px)};
    }
    const uint32_t d = small ? 88 : 208;
    fe3h_g1t::Image base, mask;
    if (small) {
        fe3h_g1t::Image frame;
        if (!piece("cmn_win_still_diamond_s_base^q", base) ||
            !piece("cmn_win_still_diamond_s_frame^q", frame) ||
            !piece("cmn_win_still_mini_mask^t", mask) || base.width != d || frame.width != d ||
            mask.width != d || base.height != d || frame.height != d || mask.height != d)
            return false;
        static constexpr uint8_t Black[2][4] = {{0x00, 0x0D, 0x26, 0x00}, {0x17, 0x01, 0x00, 0x00}};
        static constexpr uint8_t Vcol[2][4] = {{0x2A, 0x89, 0xFF, 0xFF}, {0xFF, 0x54, 0x45, 0xFF}};
        Tint(base, Black[army], Vcol[army]);
        Over(base, frame, nullptr);
    } else {
        static constexpr const char* Backing[4] = {
            "cmn_win_still_diamond_blue^q", "cmn_win_still_diamond_red^q", "cmn_win_still_green^q",
            "cmn_win_still_yellow^q"};
        if (!piece(Backing[army], base) || !piece("cmn_win_still_diamond_mask^t", mask) ||
            base.width != d || base.height != d || mask.width != d || mask.height != d)
            return false;
    }
    Over(base, Resample(still, d), &mask);
    out = std::move(base);
    return true;
}

bool LibraryImpl::SpriteSource(const EdenDsmodHostApi* host, uint32_t id, fe3h_g1t::Image& tex,
                               uint32_t rect[4]) {
    std::string parent;
    {
        std::scoped_lock lock{io_};
        if (!Ready(host) ||
            !Crop(host, "fe3h:sprite/" + std::to_string(id), parent, rect[0], rect[1], rect[2], rect[3]))
            return false;
    }
    std::vector<uint8_t> px;
    uint32_t w{}, h{};
    if (!Load(host, parent, px, w, h))
        return false;
    tex = {w, h, std::move(px)};
    return true;
}

namespace {

// Bilinear sample of straight RGBA at texel-space (u, v) (texel centres at i + 0.5), neighbours
// clamped to the texture; channel = floor(value + 0.5). The Python reference
// (packaging/fe3h/assets-test/refgen_panel.py) evaluates the same expressions in doubles.
void Sample(const fe3h_g1t::Image& t, double u, double v, uint8_t out[4]) {
    const double x = u - 0.5, y = v - 0.5;
    const double fx0 = std::floor(x), fy0 = std::floor(y);
    const double fx = x - fx0, fy = y - fy0;
    auto clampi = [](double c, uint32_t n) {
        return c < 0 ? 0u : c > double(n - 1) ? n - 1 : uint32_t(c);
    };
    const uint32_t x0 = clampi(fx0, t.width), x1 = clampi(fx0 + 1, t.width);
    const uint32_t y0 = clampi(fy0, t.height), y1 = clampi(fy0 + 1, t.height);
    const uint8_t* p00 = t.rgba.data() + (size_t(y0) * t.width + x0) * 4;
    const uint8_t* p10 = t.rgba.data() + (size_t(y0) * t.width + x1) * 4;
    const uint8_t* p01 = t.rgba.data() + (size_t(y1) * t.width + x0) * 4;
    const uint8_t* p11 = t.rgba.data() + (size_t(y1) * t.width + x1) * 4;
    for (int c = 0; c < 4; ++c) {
        const double top = p00[c] * (1.0 - fx) + p10[c] * fx;
        const double bot = p01[c] * (1.0 - fx) + p11[c] * fx;
        const double val = top * (1.0 - fy) + bot * fy;
        out[c] = uint8_t(std::floor(val + 0.5));
    }
}

// Texel coordinate of output pixel p (0..n-1) along one axis of the 0x39C6C0 9-slice: the first /
// last `a` / `b` pixels copy the slices 1:1, the rest stretches the `m`-texel middle band.
double NineAxis(uint32_t p, uint32_t n, double a, double m, double b) {
    if (p < a)
        return p + 0.5;
    if (p >= n - b)
        return a + m + (p - (n - b)) + 0.5;
    return a + ((p - a) + 0.5) * m / (n - a - b);
}

} // namespace

namespace {

// Paper-under-mask windows (0x5C4EE0, 0x5C4B50, 0x5C4600): the paper sprite 0x279 (1024^2) as a
// quad at (qx, qy) qw x qh showing texels [pr.x, pr.x + us) x [pr.y, pr.y + vs), and the mask
// sprite drawn as a 9-slice over the whole w x h rect (0x5C3950 / 0x39D0A0: texel slices sx / sy,
// corners 1:1, middle band stretched). Both carry the 0x20000000 blend mode, which multiplies:
// out = paper * mask per channel (alpha included), (a * b + 127) / 255. Verified on the unit
// Details "Stats" title bar: paper ~(225,218,198) x mask 0x255 top (46,39,57) -> native (40,31,42).
void PaperMask(fe3h_g1t::Image& out, uint32_t w, uint32_t h, const fe3h_g1t::Image& paper,
               const uint32_t pr[4], uint32_t qx, uint32_t qy, uint32_t qw, uint32_t qh, double us,
               double vs, const fe3h_g1t::Image& mask, const uint32_t mr[4], const double sx[3],
               const double sy[3]) {
    out = {w, h, std::vector<uint8_t>(size_t(w) * h * 4, 0)};
    for (uint32_t y = qy; y < qy + qh; ++y) {
        const double mv = mr[1] + NineAxis(y, h, sy[0], sy[1], sy[2]);
        const double pv = pr[1] + ((y - qy) + 0.5) * vs / qh;
        for (uint32_t x = qx; x < qx + qw; ++x) {
            uint8_t pc[4], mc[4];
            Sample(paper, pr[0] + ((x - qx) + 0.5) * us / qw, pv, pc);
            Sample(mask, mr[0] + NineAxis(x, w, sx[0], sx[1], sx[2]), mv, mc);
            uint8_t* o = out.rgba.data() + (size_t(y) * w + x) * 4;
            for (int c = 0; c < 4; ++c)
                o[c] = uint8_t((pc[c] * mc[c] + 127) / 255);
        }
    }
}

} // namespace

// fe3h:panel/<w>/<h> and fe3h:panel_abyss/<w>/<h>: the People "Occupants" panel as 0x5C4EE0 draws
// it at design size w x h: sprite 0x279 (parchment 1024^2) stretched over the rect inset by 4 px,
// multiplied by the 9-slice torn-edge mask over the whole rect (see PaperMask): h >= 340 -> 0x259 /
// 0x7B54 slices 160|20|160 both ways; h < 340 -> 0x25F / 0x7B65 slices 108|16|108 across,
// 50|20|50 down. Limits: w, h <= 2048, w >= left + right, h >= top + bottom of the chosen mask.
//
// fe3h:titlewin/<w>/<h>: the titled window 0x5CB1C0 -> 0x5C4B50 (unit Details "Stats" /
// "Abilities" / "Skill Level", ...): paper 0x279 stretched over the whole rect, mask 0x255 (104 x
// 84) slices 48|8|48 across, 56|8|20 down; then the cog 0x79 (32 x 32) 1:1 at (17, 8), source-over.
// The title text (+56, +10, size 28, font 0x10, colour 0x30 outline 0x31) is left to the caller.
// w >= 104, h >= 84.
//
// fe3h:paperwin/<mask>/<w>/<h>: 0x5CAF10(mask, alpha, x, y, w, h) -> 0x5C4600 (unit Details profile
// uses mask 0x253): paper 0x279 over the whole rect at half scale (UV 0..min(2 w / 1024, 1)),
// mask 9-slice 48|8|48 both ways. The mask sprite must be >= 104 x 104; w, h >= 104.
bool LibraryImpl::Panel(const EdenDsmodHostApi* host, std::string_view key, fe3h_g1t::Image& out) {
    enum { Occupants, Titled, Paper } kind;
    std::string_view rest;
    bool abyss = false;
    if (key.starts_with("fe3h:panel_abyss/")) {
        kind = Occupants, abyss = true, rest = key.substr(17);
    } else if (key.starts_with("fe3h:panel/")) {
        kind = Occupants, rest = key.substr(11);
    } else if (key.starts_with("fe3h:titlewin/")) {
        kind = Titled, rest = key.substr(14);
    } else if (key.starts_with("fe3h:paperwin/")) {
        kind = Paper, rest = key.substr(14);
    } else {
        return false;
    }
    const auto part = Split(rest);
    uint32_t w, h, mask_id = 0;
    const size_t n = kind == Paper ? 3 : 2;
    if (part.size() != n || (kind == Paper && !ParseU32(part[0], mask_id)) ||
        !ParseU32(part[n - 2], w) || !ParseU32(part[n - 1], h) || w > 2048 || h > 2048)
        return false;
    double sx[3], sy[3];
    if (kind == Occupants) {
        const bool big = h >= 340; // main+0xCB7770 = 340.0
        sx[0] = sx[2] = big ? 160.0 : 108.0, sx[1] = big ? 20.0 : 16.0;
        sy[0] = sy[2] = big ? 160.0 : 50.0, sy[1] = 20.0;
        mask_id = abyss ? (big ? 0x7B54 : 0x7B65) : (big ? 0x259 : 0x25F);
    } else if (kind == Titled) {
        sx[0] = sx[2] = 48.0, sx[1] = 8.0, sy[0] = 56.0, sy[1] = 8.0, sy[2] = 20.0;
        mask_id = SpriteTitleWinMask;
    } else {
        sx[0] = sx[2] = sy[0] = sy[2] = 48.0, sx[1] = sy[1] = 8.0;
    }
    if (w < sx[0] + sx[2] || h < sy[0] + sy[2] || w < 16 || h < 16)
        return false;
    fe3h_g1t::Image paper, mask;
    uint32_t pr[4], mr[4];
    if (!SpriteSource(host, SpritePanelPaper, paper, pr) || !SpriteSource(host, mask_id, mask, mr))
        return false;
    if (mr[2] < sx[0] + sx[1] + sx[2] || mr[3] < sy[0] + sy[1] + sy[2])
        return false;
    if (kind == Occupants) {
        PaperMask(out, w, h, paper, pr, 4, 4, w - 8, h - 8, pr[2], pr[3], mask, mr, sx, sy);
        return true;
    }
    if (kind == Paper) {
        // main+0xCB7D8C = 1 / 1024: UV = min(size / 1024 * 2, 1) of the (whole-texture) paper.
        const double us = std::min(double(w) * (1.0 / 1024.0) * 2.0, 1.0) * pr[2];
        const double vs = std::min(double(h) * (1.0 / 1024.0) * 2.0, 1.0) * pr[3];
        PaperMask(out, w, h, paper, pr, 0, 0, w, h, us, vs, mask, mr, sx, sy);
        return true;
    }
    PaperMask(out, w, h, paper, pr, 0, 0, w, h, pr[2], pr[3], mask, mr, sx, sy);
    fe3h_g1t::Image cog;
    uint32_t cr[4];
    if (!SpriteSource(host, SpriteTitleWinCog, cog, cr) || cr[2] != 32 || cr[3] != 32)
        return false;
    for (uint32_t y = 0; y < 32 && 8 + y < h; ++y)
        for (uint32_t x = 0; x < 32 && 17 + x < w; ++x) {
            const uint8_t* s = cog.rgba.data() + (size_t(cr[1] + y) * cog.width + cr[0] + x) * 4;
            uint8_t* d = out.rgba.data() + (size_t(8 + y) * w + 17 + x) * 4;
            const double sa = s[3] / 255.0, da = d[3] / 255.0, oa = sa + da * (1.0 - sa);
            if (oa <= 0.0)
                continue;
            for (int c = 0; c < 3; ++c)
                d[c] = uint8_t(std::floor((s[c] * sa + d[c] * da * (1.0 - sa)) / oa + 0.5));
            d[3] = uint8_t(std::floor(oa * 255.0 + 0.5));
        }
    return true;
}

// fe3h:nine/<id>/<w>/<h>/<l>/<m>/<r>[/<t>/<mv>/<b>]: sprite <id> drawn as the game's 9-slice
// (0x5C3950 / 0x5CAD90 -> 0x39D0A0) at w x h: texel slices l|m|r across and t|mv|b down (corners
// 1:1, middle bands stretched; see NineAxis). With only l|m|r the height is a plain stretch of the
// whole sprite (the 3-slice 0x5C41D0 drawn at the sprite's own height is then 1:1 vertically).
// Slices must lie inside the sprite; w >= l + r, h >= t + b, w, h <= 2048.
bool LibraryImpl::Nine(const EdenDsmodHostApi* host, std::string_view key, fe3h_g1t::Image& out) {
    const auto part = Split(key.substr(10));
    if (part.size() != 6 && part.size() != 9)
        return false;
    uint32_t v[9]{};
    for (size_t i = 0; i < part.size(); ++i)
        if (!ParseU32(part[i], v[i]))
            return false;
    const uint32_t w = v[1], h = v[2];
    if (w == 0 || h == 0 || w > 2048 || h > 2048)
        return false;
    fe3h_g1t::Image tex;
    uint32_t r[4];
    if (!SpriteSource(host, v[0], tex, r))
        return false;
    const double sx[3] = {double(v[3]), double(v[4]), double(v[5])};
    const double sy[3] = {part.size() == 9 ? double(v[6]) : 0.0,
                          part.size() == 9 ? double(v[7]) : double(r[3]),
                          part.size() == 9 ? double(v[8]) : 0.0};
    if (sx[0] + sx[1] + sx[2] > r[2] || sy[0] + sy[1] + sy[2] > r[3] || w < sx[0] + sx[2] ||
        h < sy[0] + sy[2] || sx[1] <= 0 || sy[1] <= 0)
        return false;
    out = {w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
    for (uint32_t y = 0; y < h; ++y) {
        const double tv = r[1] + NineAxis(y, h, sy[0], sy[1], sy[2]);
        for (uint32_t x = 0; x < w; ++x)
            Sample(tex, r[0] + NineAxis(x, w, sx[0], sx[1], sx[2]), tv,
                   out.rgba.data() + (size_t(y) * w + x) * 4);
    }
    return true;
}

// ---- the v5 redesign's composed keys (LibraryImpl::Styled) --------------------------------------
// The approved redesign (research/fe3h/redesign: STYLE.md, notes-*.md, mock/page_*.py) defines these
// images with Pillow 12 + numpy. The keys reproduce that pipeline with Pillow's own arithmetic so a
// key renders like its mockup component (checked against the mock code by
// packaging/fe3h/assets-test/refgen_styled.py):
//   resize   Resample.c: separable convolution (bilinear / lanczos filters, support scaled when
//            shrinking), 22-bit fixed-point coefficients, horizontal pass first with an 8-bit
//            intermediate; RGBA goes through premultiplied RGBa (Image.resize).
//   blur     ImageFilter.GaussianBlur(r): BoxBlur.c, 3 extended box passes per axis (horizontal
//            first), 24-bit fixed point; on the RGB channels only (the mock blurs convert("RGB")
//            and keeps the source alpha).
//   over     Image.alpha_composite (AlphaComposite.c, 7-bit precision).
//   numpy    float32 multiplies / blends, then clip(0, 255) and truncation to uint8.
namespace {

using Img = fe3h_g1t::Image;

struct PilFilter {
    double support;
    double (*fn)(double);
};
double PilBilinearFn(double x) {
    if (x < 0.0)
        x = -x;
    return x < 1.0 ? 1.0 - x : 0.0;
}
double PilSinc(double x) {
    if (x == 0.0)
        return 1.0;
    x *= 3.14159265358979323846;
    return std::sin(x) / x;
}
double PilLanczosFn(double x) {
    return (-3.0 <= x && x < 3.0) ? PilSinc(x) * PilSinc(x / 3) : 0.0;
}
constexpr PilFilter PilBilinear{1.0, PilBilinearFn}, PilLanczos{3.0, PilLanczosFn};

struct PilCoeffs {
    int ksize{};
    std::vector<int> bounds; // xmin, count per output pixel
    std::vector<int32_t> k;  // ksize per output pixel, PRECISION_BITS 22
};

PilCoeffs PilPrecompute(int in_size, int out_size, const PilFilter& f) {
    const double scale = double(in_size) / out_size;
    const double fs = scale < 1.0 ? 1.0 : scale;
    const double support = f.support * fs;
    PilCoeffs c;
    c.ksize = int(std::ceil(support)) * 2 + 1;
    c.bounds.resize(size_t(out_size) * 2);
    c.k.assign(size_t(out_size) * c.ksize, 0);
    std::vector<double> pre(c.ksize);
    for (int xx = 0; xx < out_size; ++xx) {
        const double center = (xx + 0.5) * scale;
        const double ss = 1.0 / fs;
        double ww = 0.0;
        int xmin = int(center - support + 0.5);
        if (xmin < 0)
            xmin = 0;
        int xmax = int(center + support + 0.5);
        if (xmax > in_size)
            xmax = in_size;
        xmax -= xmin;
        for (int x = 0; x < xmax; ++x) {
            pre[x] = f.fn((x + xmin - center + 0.5) * ss);
            ww += pre[x];
        }
        for (int x = 0; x < xmax; ++x) {
            if (ww != 0.0)
                pre[x] /= ww;
            c.k[size_t(xx) * c.ksize + x] = pre[x] < 0 ? int32_t(-0.5 + pre[x] * (1 << 22))
                                                       : int32_t(0.5 + pre[x] * (1 << 22));
        }
        c.bounds[size_t(xx) * 2] = xmin;
        c.bounds[size_t(xx) * 2 + 1] = xmax;
    }
    return c;
}

uint8_t PilClip8(int32_t v) {
    if (v >= (1 << 22 << 8))
        return 255;
    if (v <= 0)
        return 0;
    return uint8_t(v >> 22);
}

// One axis of Resample.c over 4-byte pixels; `horizontal` = along x.
std::vector<uint8_t> PilPass(const std::vector<uint8_t>& in, uint32_t w, uint32_t h, uint32_t n,
                             bool horizontal, const PilFilter& f) {
    const PilCoeffs c = PilPrecompute(int(horizontal ? w : h), int(n), f);
    const uint32_t ow = horizontal ? n : w, oh = horizontal ? h : n;
    std::vector<uint8_t> out(size_t(ow) * oh * 4);
    for (uint32_t y = 0; y < oh; ++y)
        for (uint32_t x = 0; x < ow; ++x) {
            const uint32_t o = horizontal ? x : y;
            const int xmin = c.bounds[size_t(o) * 2], cnt = c.bounds[size_t(o) * 2 + 1];
            const int32_t* k = &c.k[size_t(o) * c.ksize];
            int32_t ss[4] = {1 << 21, 1 << 21, 1 << 21, 1 << 21};
            for (int t = 0; t < cnt; ++t) {
                const uint8_t* p = horizontal ? &in[(size_t(y) * w + xmin + t) * 4]
                                              : &in[(size_t(xmin + t) * w + x) * 4];
                for (int ch = 0; ch < 4; ++ch)
                    ss[ch] += int32_t(p[ch]) * k[t];
            }
            for (int ch = 0; ch < 4; ++ch)
                out[(size_t(y) * ow + x) * 4 + ch] = PilClip8(ss[ch]);
        }
    return out;
}

// Image.resize((dw, dh), filter) of an RGBA image.
Img PilResize(const Img& src, uint32_t dw, uint32_t dh, const PilFilter& f) {
    if (dw == src.width && dh == src.height)
        return src;
    std::vector<uint8_t> a(src.rgba);
    for (size_t i = 0; i < a.size(); i += 4) // RGBA -> RGBa (MULDIV255)
        for (int ch = 0; ch < 3; ++ch) {
            const uint32_t t = uint32_t(a[i + ch]) * a[i + 3] + 128;
            a[i + ch] = uint8_t(((t >> 8) + t) >> 8);
        }
    uint32_t w = src.width, h = src.height;
    if (dw != w) {
        a = PilPass(a, w, h, dw, true, f);
        w = dw;
    }
    if (dh != h) {
        a = PilPass(a, w, h, dh, false, f);
        h = dh;
    }
    for (size_t i = 0; i < a.size(); i += 4) { // RGBa -> RGBA
        const uint32_t al = a[i + 3];
        if (al != 0 && al != 255)
            for (int ch = 0; ch < 3; ++ch)
                a[i + ch] = uint8_t(std::min<uint32_t>(255, 255u * a[i + ch] / al));
    }
    return {w, h, std::move(a)};
}

Img PilCrop(const Img& s, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    Img o{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
    for (uint32_t r = 0; r < h; ++r)
        std::memcpy(o.rgba.data() + size_t(r) * w * 4, s.rgba.data() + (size_t(y + r) * s.width + x) * 4,
                    size_t(w) * 4);
    return o;
}

void PilPaste(Img& d, const Img& s, uint32_t x, uint32_t y) { // plain paste (no mask)
    for (uint32_t r = 0; r < s.height; ++r)
        std::memcpy(d.rgba.data() + (size_t(y + r) * d.width + x) * 4, s.rgba.data() + size_t(r) * s.width * 4,
                    size_t(s.width) * 4);
}

// draw.stretch9 (the mock's 'nine'): slices a | mid | b across and t | mid | bb down, the corner
// pieces copied, the others resized (bilinear) to their destination size and pasted.
Img PilStretch9(const Img& im, uint32_t w, uint32_t h, uint32_t a, uint32_t b, uint32_t t, uint32_t bb) {
    Img out{w, h, std::vector<uint8_t>(size_t(w) * h * 4, 0)};
    const uint32_t W = im.width, H = im.height;
    const uint32_t xs[3][4] = {{0, a, 0, a}, {a, W - b, a, w - b}, {W - b, W, w - b, w}};
    const uint32_t ys[3][4] = {{0, t, 0, t}, {t, H - bb, t, h - bb}, {H - bb, H, h - bb, h}};
    for (const auto& X : xs)
        for (const auto& Y : ys) {
            if (X[1] <= X[0] || Y[1] <= Y[0] || X[3] <= X[2] || Y[3] <= Y[2])
                continue;
            const Img piece = PilCrop(im, X[0], Y[0], X[1] - X[0], Y[1] - Y[0]);
            PilPaste(out, PilResize(piece, X[3] - X[2], Y[3] - Y[2], PilBilinear), X[2], Y[2]);
        }
    return out;
}

float PilGaussRadius(float radius, int passes) {
    const float sigma2 = radius * radius / passes;
    const float L = std::sqrt(12.0 * sigma2 + 1.0);
    const float l = std::floor((L - 1.0) / 2.0);
    float a = (2 * l + 1) * (l * (l + 1) - 3 * sigma2);
    a /= 6 * (sigma2 - (l + 1) * (l + 1));
    return l + a;
}

// ImagingLineBoxBlur on one line of n pixels, `ch` interleaved channels of which the first `use`
// are blurred.
void PilBoxLine(const uint8_t* in, uint8_t* out, int n, int ch, int use, int radius, uint32_t ww,
                uint32_t fw) {
    const int lastx = n - 1;
    const int edge_a = std::min(radius + 1, n), edge_b = std::max(n - radius - 1, 0);
    for (int c = 0; c < use; ++c) {
        const auto px = [&](int x) -> uint32_t { return in[size_t(x) * ch + c]; };
        uint32_t acc = px(0) * uint32_t(radius + 1);
        for (int x = 0; x < edge_a - 1; ++x)
            acc += px(x);
        acc += px(lastx) * uint32_t(radius - edge_a + 1);
        const auto step = [&](int x, int sub, int add, int fl, int fr) {
            acc += px(add) - px(sub);
            const uint32_t bulk = acc * ww + (px(fl) + px(fr)) * fw;
            out[size_t(x) * ch + c] = uint8_t((bulk + (1u << 23)) >> 24);
        };
        if (edge_a <= edge_b) {
            for (int x = 0; x < edge_a; ++x)
                step(x, 0, x + radius, 0, x + radius + 1);
            for (int x = edge_a; x < edge_b; ++x)
                step(x, x - radius - 1, x + radius, x - radius - 1, x + radius + 1);
            for (int x = edge_b; x <= lastx; ++x)
                step(x, x - radius - 1, lastx, x - radius - 1, lastx);
        } else {
            for (int x = 0; x < edge_b; ++x)
                step(x, 0, x + radius, 0, x + radius + 1);
            for (int x = edge_b; x < edge_a; ++x)
                step(x, 0, lastx, 0, lastx);
            for (int x = edge_a; x <= lastx; ++x)
                step(x, x - radius - 1, lastx, x - radius - 1, lastx);
        }
    }
}

// ImageFilter.GaussianBlur(radius) on the first `use` of `ch` interleaved channels of a w x h
// buffer (in place).
void PilGaussian(std::vector<uint8_t>& px, uint32_t w, uint32_t h, int ch, int use, float radius) {
    if (radius <= 0.0f || w == 0 || h == 0)
        return;
    const float r = PilGaussRadius(radius, 3);
    if (r == 0.0f)
        return;
    const int ri = int(r);
    const uint32_t ww = uint32_t(float(1u << 24) / (r * 2 + 1));
    const uint32_t fw = ((1u << 24) - uint32_t(ri * 2 + 1) * ww) / 2;
    std::vector<uint8_t> line(size_t(std::max(w, h)) * ch), res(line.size());
    for (int pass = 0; pass < 3; ++pass)
        for (uint32_t y = 0; y < h; ++y) {
            uint8_t* row = px.data() + size_t(y) * w * ch;
            std::memcpy(line.data(), row, size_t(w) * ch);
            std::memcpy(res.data(), row, size_t(w) * ch);
            PilBoxLine(line.data(), res.data(), int(w), ch, use, ri, ww, fw);
            std::memcpy(row, res.data(), size_t(w) * ch);
        }
    for (int pass = 0; pass < 3; ++pass)
        for (uint32_t x = 0; x < w; ++x) {
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy(&line[size_t(y) * ch], &px[(size_t(y) * w + x) * ch], size_t(ch));
            std::memcpy(res.data(), line.data(), size_t(h) * ch);
            PilBoxLine(line.data(), res.data(), int(h), ch, use, ri, ww, fw);
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy(&px[(size_t(y) * w + x) * ch], &res[size_t(y) * ch], size_t(ch));
        }
}

// Image.alpha_composite(src at (x, y)) onto dst (src clipped to dst).
void PilOver(Img& dst, const Img& src, int32_t x, int32_t y) {
    for (uint32_t sy = 0; sy < src.height; ++sy)
        for (uint32_t sx = 0; sx < src.width; ++sx) {
            const int64_t dx = int64_t(x) + sx, dy = int64_t(y) + sy;
            if (dx < 0 || dy < 0 || dx >= dst.width || dy >= dst.height)
                continue;
            const uint8_t* s = &src.rgba[(size_t(sy) * src.width + sx) * 4];
            uint8_t* d = &dst.rgba[(size_t(dy) * dst.width + dx) * 4];
            if (s[3] == 0)
                continue;
            const uint32_t blend = uint32_t(d[3]) * (255 - s[3]);
            const uint32_t outa255 = uint32_t(s[3]) * 255 + blend;
            const uint32_t coef1 = uint32_t(s[3]) * 255 * 255 * (1 << 7) / outa255;
            const uint32_t coef2 = 255 * (1 << 7) - coef1;
            for (int c = 0; c < 3; ++c) {
                const uint32_t t = s[c] * coef1 + d[c] * coef2 + (0x80 << 7);
                d[c] = uint8_t(((((t >> 8) + t) >> 8)) >> 7);
            }
            const uint32_t ta = outa255 + 0x80;
            d[3] = uint8_t(((ta >> 8) + ta) >> 8);
        }
}

// float32 RGB multiply then clip + truncation (numpy `a[..., :3] *= dim; astype(uint8)`).
void NpDim(std::vector<uint8_t>& px, float dim) {
    for (size_t i = 0; i < px.size(); i += 4)
        for (int c = 0; c < 3; ++c) {
            const float v = float(px[i + c]) * dim;
            px[i + c] = uint8_t(std::clamp(v, 0.0f, 255.0f));
        }
}

// The leading numeric fields of a composed key and the inner key after them.
bool StyledArgs(std::string_view rest, size_t n, uint32_t* v, std::string_view* inner) {
    for (size_t i = 0; i < n; ++i) {
        const size_t q = rest.find('/');
        if (q == std::string_view::npos && !(i + 1 == n && !inner))
            return false;
        if (!ParseU32(rest.substr(0, q), v[i]))
            return false;
        rest = q == std::string_view::npos ? std::string_view{} : rest.substr(q + 1);
    }
    if (inner) {
        *inner = rest;
        return !rest.empty();
    }
    return rest.empty();
}

} // namespace

bool LibraryImpl::Inner(const EdenDsmodHostApi* host, std::string_view key, Img& out) {
    std::vector<uint8_t> px;
    uint32_t w{}, h{};
    if (key.starts_with("fe3h:") || key.starts_with("module:") ||
        !Load(host, "fe3h:" + std::string(key), px, w, h))
        return false;
    out = {w, h, std::move(px)};
    return true;
}

// Composed keys of the v5 redesign (sizes in design px; <dim%> etc. are integer percent; <key> = any
// other image key without "fe3h:", e.g. sprite/1910):
//   fe3h:bg/<w>/<h>/<sl>/<blur>/<dim%>/<frame>/<fdim%>/<feather>/<key>
//        screen background (page_chrome.bg_image): <key> 9-sliced (stretch9, slices <sl> on all four
//        sides) to w x h; RGB gaussian-blurred <blur> and x dim; the outer ring (distance to the
//        nearest edge < frame) blended back to the sharp board x fdim over <feather> px:
//        k = clip((d - frame) / feather + 0.5, 0, 1), out = sharp * fdim * (1 - k) + soft * k.
//        frame 0 = no ring. Chrome v4/v5: fe3h:bg/1240/1080/70/4/72/64/100/24/sprite/1910.
//   fe3h:blur/<r>/<dim%>/<key>           soften(): RGB blurred <r>, x dim (alpha kept).
//   fe3h:win/<w>/<h>/<sl>/<inset>/<r>/<key>
//        purple_window_img(): <key> stretch9'd (slices <sl>) to w x h, the interior (inset px from
//        every edge) replaced by its own RGB blur <r>, opaque. War map frame / forecast:
//        fe3h:win/<w>/<h>/40/34/80/sprite/2133.
//   fe3h:patch/<x>/<y>/<w>/<h>/<blur>/<dim%>/<radius>/<feather>/<key>
//        soft_patch(): the w x h rect at (x, y) of <key> (clipped to it), RGB blurred, x dim, alpha =
//        a rounded rectangle (corner radius, inset by feather) blurred by feather. Tab-strip patch:
//        .../8/80/26/8/bg/....
//   fe3h:mirror/<b>/<key>    <key> with its outer b px replaced by the interior mirrored outward
//                            (numpy pad 'symmetric'): the Monastery board 2133 without its rope.
//   fe3h:crop/<x>/<y>/<w>/<h>/<key>      a pixel rect of <key>.
//   fe3h:num/<style>/<h>/<digits>[/<W>/<l|c|r>]
//        draw.big_number(): the calendar digit sprites (style ivory 1886.. / red 1897.., 's' = their
//        slash 1896 / 1907; white = battle digits 1640.., no slash), each cropped to its alpha
//        columns and resized (lanczos) to height h, advanced by its width + round(-0.04 h), composited
//        left to right. Without W the image is the run's own width; with W it is W wide and the
//        run is aligned left / centre / right in it. h <= 512, W <= 2048, <= 8 glyphs.
bool LibraryImpl::Styled(const EdenDsmodHostApi* host, std::string_view key, Img& out) {
    key.remove_prefix(5); // "fe3h:"
    const size_t slash = key.find('/');
    const std::string_view kind = key.substr(0, slash), rest = key.substr(slash + 1);
    std::string_view inner;
    uint32_t v[9]{};
    if (kind == "scrolltrack") {
        if (!StyledArgs(rest, 2, v, nullptr) || v[0] == 0 || v[1] < 48 || v[0] > 128 || v[1] > 2048)
            return false;
        Img src;
        if (!Inner(host, "sprite/1839", src)) return false;
        Img turned{src.height, src.width, std::vector<uint8_t>(src.rgba.size())};
        for (uint32_t y = 0; y < src.height; ++y)
            for (uint32_t x = 0; x < src.width; ++x)
                std::copy_n(src.rgba.data() + (size_t(y) * src.width + x) * 4, 4,
                    turned.rgba.data() + (size_t(src.width - 1 - x) * turned.width + y) * 4);
        out = PilStretch9(turned, v[0], v[1], 0, 0, 24, 24);
        return true;
    }
    if (kind == "bg") {
        if (!StyledArgs(rest, 8, v, &inner))
            return false;
        const uint32_t w = v[0], h = v[1], sl = v[2], frame = v[5], feather = v[7];
        if (w == 0 || h == 0 || w > 2048 || h > 2048 || v[3] > 200 || v[4] > 100 || v[6] > 100)
            return false;
        Img src;
        if (!Inner(host, inner, src) || 2 * sl >= src.width || 2 * sl >= src.height || w < 2 * sl ||
            h < 2 * sl)
            return false;
        const Img sharp = PilStretch9(src, w, h, sl, sl, sl, sl);
        std::vector<uint8_t> soft = sharp.rgba;
        PilGaussian(soft, w, h, 4, 3, float(v[3]));
        const float dim = float(v[4] / 100.0), fdim = float(v[6] / 100.0);
        out = {w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = (size_t(y) * w + x) * 4;
                float k = 1.0f;
                if (frame > 0) {
                    const float d = float(std::min(std::min(x, w - 1 - x), std::min(y, h - 1 - y)));
                    const float t = (d - float(frame)) / float(std::max<uint32_t>(1, feather));
                    k = std::clamp(t + 0.5f, 0.0f, 1.0f);
                }
                for (int c = 0; c < 4; ++c) {
                    const float a = c < 3 ? float(soft[i + c]) * dim : float(soft[i + c]);
                    float r = a;
                    if (frame > 0) {
                        const float s = c < 3 ? float(sharp.rgba[i + c]) * fdim : float(sharp.rgba[i + c]);
                        const float one_k = 1.0f - k;
                        const float p = s * one_k;
                        const float q = a * k;
                        r = p + q;
                    }
                    out.rgba[i + c] = uint8_t(std::clamp(r, 0.0f, 255.0f));
                }
            }
        return true;
    }
    if (kind == "blur") {
        if (!StyledArgs(rest, 2, v, &inner) || v[0] > 200 || v[1] > 100 || !Inner(host, inner, out))
            return false;
        PilGaussian(out.rgba, out.width, out.height, 4, 3, float(v[0]));
        if (v[1] < 100)
            NpDim(out.rgba, float(v[1] / 100.0));
        return true;
    }
    if (kind == "win") {
        if (!StyledArgs(rest, 5, v, &inner))
            return false;
        const uint32_t w = v[0], h = v[1], sl = v[2], in = v[3];
        Img src;
        if (w == 0 || h == 0 || w > 2048 || h > 2048 || v[4] > 200 || 2 * in >= w || 2 * in >= h ||
            !Inner(host, inner, src) || 2 * sl >= src.width || 2 * sl >= src.height || w < 2 * sl ||
            h < 2 * sl)
            return false;
        out = PilStretch9(src, w, h, sl, sl, sl, sl);
        Img box = PilCrop(out, in, in, w - 2 * in, h - 2 * in);
        PilGaussian(box.rgba, box.width, box.height, 4, 3, float(v[4]));
        for (size_t i = 3; i < box.rgba.size(); i += 4)
            box.rgba[i] = 255;
        PilPaste(out, box, in, in);
        return true;
    }
    if (kind == "patch") {
        if (!StyledArgs(rest, 8, v, &inner))
            return false;
        Img src;
        if (v[2] == 0 || v[3] == 0 || v[4] > 200 || v[5] > 100 || !Inner(host, inner, src) ||
            v[0] >= src.width || v[1] >= src.height)
            return false;
        const uint32_t x0 = v[0], y0 = v[1], w = std::min(v[2], src.width - x0),
                       h = std::min(v[3], src.height - y0);
        const uint32_t rad = v[6], fe = v[7];
        out = PilCrop(src, x0, y0, w, h);
        PilGaussian(out.rgba, w, h, 4, 3, float(v[4]));
        NpDim(out.rgba, float(v[5] / 100.0));
        // ImageDraw.rounded_rectangle((fe, fe, w - 1 - fe, h - 1 - fe), radius, fill=255), then
        // GaussianBlur(fe) on the L mask
        std::vector<uint8_t> m(size_t(w) * h, 0);
        const double rx0 = fe, ry0 = fe, rx1 = double(w) - 1 - fe, ry1 = double(h) - 1 - fe;
        const double r = std::min<double>(rad, std::min(rx1 - rx0, ry1 - ry0) / 2);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                if (x < rx0 || x > rx1 || y < ry0 || y > ry1)
                    continue;
                const double cx = std::clamp<double>(x, rx0 + r, rx1 - r);
                const double cy = std::clamp<double>(y, ry0 + r, ry1 - r);
                const double dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy <= (r + 0.5) * (r + 0.5))
                    m[size_t(y) * w + x] = 255;
            }
        PilGaussian(m, w, h, 1, 1, float(fe));
        for (size_t i = 0; i < m.size(); ++i)
            out.rgba[i * 4 + 3] = m[i];
        return true;
    }
    if (kind == "mirror") {
        Img src;
        if (!StyledArgs(rest, 1, v, &inner) || !Inner(host, inner, src) || v[0] == 0 ||
            4 * v[0] > src.width || 4 * v[0] > src.height)
            return false;
        const int64_t b = v[0], n = int64_t(src.width) - 2 * b, m = int64_t(src.height) - 2 * b;
        const auto fold = [](int64_t i, int64_t len) {
            return i < 0 ? -i - 1 : i >= len ? 2 * len - 1 - i : i;
        };
        out = {src.width, src.height, std::vector<uint8_t>(src.rgba.size())};
        for (uint32_t y = 0; y < src.height; ++y)
            for (uint32_t x = 0; x < src.width; ++x) {
                const int64_t sx = b + fold(int64_t(x) - b, n), sy = b + fold(int64_t(y) - b, m);
                std::memcpy(&out.rgba[(size_t(y) * src.width + x) * 4],
                            &src.rgba[(size_t(sy) * src.width + sx) * 4], 4);
            }
        return true;
    }
    if (kind == "crop") {
        Img src;
        if (!StyledArgs(rest, 4, v, &inner) || v[2] == 0 || v[3] == 0 || !Inner(host, inner, src) ||
            uint64_t(v[0]) + v[2] > src.width || uint64_t(v[1]) + v[3] > src.height)
            return false;
        out = PilCrop(src, v[0], v[1], v[2], v[3]);
        return true;
    }
    if (kind == "num") {
        const auto part = Split(rest);
        if (part.size() != 3 && part.size() != 5)
            return false;
        uint32_t h, cw = 0;
        if (!ParseU32(part[1], h) || h == 0 || h > 512 || part[2].empty() || part[2].size() > 8)
            return false;
        uint32_t d0, sl;
        if (part[0] == "ivory")
            d0 = 1886, sl = 1896;
        else if (part[0] == "red")
            d0 = 1897, sl = 1907;
        else if (part[0] == "white")
            d0 = 1640, sl = 0;
        else
            return false;
        char align = 'l';
        if (part.size() == 5) {
            if (!ParseU32(part[3], cw) || cw == 0 || cw > 2048 || part[4].size() != 1 ||
                std::string_view{"lcr"}.find(part[4][0]) == std::string_view::npos)
                return false;
            align = part[4][0];
        }
        std::vector<Img> glyphs;
        for (const char ch : part[2]) {
            uint32_t id;
            if (ch >= '0' && ch <= '9')
                id = d0 + uint32_t(ch - '0');
            else if (ch == 's' && sl)
                id = sl;
            else
                return false;
            Img g;
            if (!Inner(host, "sprite/" + std::to_string(id), g))
                return false;
            // getbbox() (alpha): the glyph's opaque columns
            uint32_t bx0 = g.width, bx1 = 0;
            for (uint32_t y = 0; y < g.height; ++y)
                for (uint32_t x = 0; x < g.width; ++x)
                    if (g.rgba[(size_t(y) * g.width + x) * 4 + 3]) {
                        bx0 = std::min(bx0, x);
                        bx1 = std::max(bx1, x + 1);
                    }
            if (bx1 <= bx0)
                bx0 = 0, bx1 = g.width;
            g = PilCrop(g, bx0, 0, bx1 - bx0, g.height);
            const double k = double(h) / g.height;
            const uint32_t gw = std::max<uint32_t>(1, uint32_t(std::nearbyint(g.width * k)));
            glyphs.push_back(PilResize(g, gw, h, PilLanczos));
        }
        const int32_t sp = int32_t(std::nearbyint(-0.04 * h));
        int32_t tw = sp * int32_t(glyphs.size() - 1);
        for (const auto& g : glyphs)
            tw += int32_t(g.width);
        if (tw <= 0)
            return false;
        const uint32_t ow = cw ? cw : uint32_t(tw);
        int32_t x = align == 'r' ? int32_t(ow) - tw : align == 'c' ? int32_t(ow / 2) - tw / 2 : 0;
        out = {ow, h, std::vector<uint8_t>(size_t(ow) * h * 4, 0)};
        for (const auto& g : glyphs) {
            PilOver(out, g, x, 0);
            x += int32_t(g.width) + sp;
        }
        return true;
    }
    return false;
}

namespace {

// Crop of a sprite out of its atlas texture (1:1).
fe3h_g1t::Image CropRect(const fe3h_g1t::Image& t, const uint32_t r[4]) {
    fe3h_g1t::Image o{r[2], r[3], std::vector<uint8_t>(size_t(r[2]) * r[3] * 4)};
    for (uint32_t y = 0; y < r[3]; ++y)
        std::memcpy(o.rgba.data() + size_t(y) * r[2] * 4,
                    t.rgba.data() + (size_t(r[1] + y) * t.width + r[0]) * 4, size_t(r[2]) * 4);
    return o;
}

// Straight-alpha "source over" of `src` (optionally scaled to dw x dh with Sample, and its alpha
// multiplied by `mask`, both placed at (dx, dy) on the canvas / (mx, my) for the mask) onto dst.
// Expressions shared with refgen_portrait.py (doubles, floor(v + 0.5)).
void Draw(fe3h_g1t::Image& dst, const fe3h_g1t::Image& src, int32_t dx, int32_t dy, uint32_t dw,
          uint32_t dh, const fe3h_g1t::Image* mask, int32_t mx, int32_t my) {
    for (int32_t y = 0; y < int32_t(dst.height); ++y)
        for (int32_t x = 0; x < int32_t(dst.width); ++x) {
            const int32_t sx = x - dx, sy = y - dy;
            if (sx < 0 || sy < 0 || sx >= int32_t(dw) || sy >= int32_t(dh))
                continue;
            uint8_t s[4];
            if (dw == src.width && dh == src.height) {
                std::memcpy(s, src.rgba.data() + (size_t(sy) * src.width + sx) * 4, 4);
            } else {
                Sample(src, (sx + 0.5) * src.width / dw, (sy + 0.5) * src.height / dh, s);
            }
            double sa = s[3] / 255.0;
            if (mask) {
                const int32_t qx = x - mx, qy = y - my;
                const bool in = qx >= 0 && qy >= 0 && qx < int32_t(mask->width) &&
                                qy < int32_t(mask->height);
                sa = in ? sa * (mask->rgba[(size_t(qy) * mask->width + qx) * 4 + 3] / 255.0) : 0.0;
            }
            uint8_t* d = dst.rgba.data() + (size_t(y) * dst.width + x) * 4;
            const double da = d[3] / 255.0;
            const double oa = sa + da * (1.0 - sa);
            if (oa <= 0.0) {
                d[0] = d[1] = d[2] = d[3] = 0;
                continue;
            }
            for (int c = 0; c < 3; ++c)
                d[c] = uint8_t(std::floor((s[c] * sa + d[c] * da * (1.0 - sa)) / oa + 0.5));
            d[3] = uint8_t(std::floor(oa * 255.0 + 0.5));
        }
}

} // namespace

// Portrait frames composed from their draw code (1080p design pixels, straight RGBA):
//   fe3h:sport/<army>/<face key>   battle unit-window portrait (backing 0x5C8E10 + face/mask
//                                  0x135780): 256 x 256 canvas; backing army 0 = 618 (blue),
//                                  1 / 4 = 621 (red), 2 = 619 (green), 3 = 627 (yellow), "grey"
//                                  = 628 (0x5C8E10 w3); face (still) 256 x 256 at (0, 0) with
//                                  its alpha x mask 620 at (0, 0).
//   fe3h:sport_s/<army>/<face key> the small variant (w2 = 1): 200 x 200; backing 622 / 625 /
//                                  623 / 626 ("grey" -> 622); face at (7, 15) size 204 x 204,
//                                  alpha x mask 624 at (0, 0) (outside the mask -> clipped).
//   fe3h:oport/<face key>          the oval roster portrait (0x5D5230): canvas 236 x 248 =
//                                  union of the pieces placed relative to the caller's origin
//                                  (+40, +4): backing 616 (228 x 248) at (4, 0), face 224 x 224
//                                  at (6, 15), alpha x mask 617 (236 x 236) at (0, 9).
// <face key> as for fe3h:dport (still/..., portrait/..., face/..., faceidx/..., facerow/...,
// unitface/...).
bool LibraryImpl::Portrait(const EdenDsmodHostApi* host, std::string_view key,
                           fe3h_g1t::Image& out) {
    key.remove_prefix(5); // "fe3h:"
    const bool oval = key.starts_with("oport/");
    const bool small = key.starts_with("sport_s/");
    const bool adj = key.starts_with("adjport/");
    key.remove_prefix(adj ? 8 : oval ? 6 : small ? 8 : 6);
    int army = 0;
    bool grey = false;
    if (!oval && !adj) {
        const size_t slash = key.find('/');
        if (slash == std::string_view::npos)
            return false;
        const std::string_view a = key.substr(0, slash);
        uint32_t v;
        if (a == "grey")
            grey = true;
        else if (ParseU32(a, v) && v <= 4)
            army = int(v);
        else
            return false;
        key.remove_prefix(slash + 1);
    }
    static constexpr std::string_view FaceKinds[] = {"portrait/", "face/", "faceidx/",
                                                     "facerow/", "unitface/", "still/"};
    bool allowed = false;
    for (const auto k : FaceKinds)
        allowed = allowed || key.starts_with(k);
    if (!allowed)
        return false;
    fe3h_g1t::Image face;
    {
        std::vector<uint8_t> px;
        uint32_t w{}, h{};
        if (!Load(host, "fe3h:" + std::string(key), px, w, h))
            return false;
        face = {w, h, std::move(px)};
    }
    auto sprite = [&](uint32_t id, fe3h_g1t::Image& img) {
        fe3h_g1t::Image tex;
        uint32_t r[4];
        if (!SpriteSource(host, id, tex, r))
            return false;
        img = CropRect(tex, r);
        return true;
    };
    fe3h_g1t::Image backing, mask;
    if (adj) {
        if (!sprite(SpriteAdjFrame, backing) || !sprite(SpriteAdjMask, mask) ||
            backing.width != 128 || backing.height != 128 || mask.width != 128 ||
            mask.height != 128)
            return false;
        out = {128, 128, std::vector<uint8_t>(size_t(128) * 128 * 4, 0)};
        Draw(out, backing, 0, 0, 128, 128, nullptr, 0, 0);
        Draw(out, face, -7, -11, 144, 144, &mask, 0, 0);
        return true;
    }
    if (oval) {
        if (!sprite(0x268, backing) || !sprite(0x269, mask))
            return false;
        out = {236, 248, std::vector<uint8_t>(size_t(236) * 248 * 4, 0)};
        Draw(out, backing, 4, 0, backing.width, backing.height, nullptr, 0, 0);
        Draw(out, face, 6, 15, 224, 224, &mask, 0, 9);
        return true;
    }
    static constexpr uint32_t Large[5] = {0x26A, 0x26D, 0x26B, 0x273, 0x26D};
    static constexpr uint32_t Small[5] = {0x26E, 0x271, 0x26F, 0x272, 0x271};
    const uint32_t back_id = grey ? (small ? 0x26E : 0x274) : (small ? Small[army] : Large[army]);
    if (!sprite(back_id, backing) || !sprite(small ? 0x270 : 0x26C, mask))
        return false;
    const uint32_t d = small ? 200 : 256;
    out = {d, d, std::vector<uint8_t>(size_t(d) * d * 4, 0)};
    Draw(out, backing, 0, 0, backing.width, backing.height, nullptr, 0, 0);
    if (small)
        Draw(out, face, 7, 15, 204, 204, &mask, 0, 0);
    else
        Draw(out, face, 0, 0, 256, 256, &mask, 0, 0);
    return true;
}

namespace {

// A sprite quad as 0x5C7850 submits it: the whole sprite rect (UVs of the rect) stretched over
// the destination x0..x0+w, y0..y0+h (float), vertex colours v0 TL, v1 TR, v2 BL, v3 BR (RGBA
// bytes) interpolated at pixel centres and multiplied into the bilinear texel, composited
// "source over". Pixels are covered when their centre lies inside the rect (top-left rule).
// Expressions shared with refgen_gauge.py.
void ColourQuad(fe3h_g1t::Image& dst, const fe3h_g1t::Image& tex, const uint32_t r[4], double x0,
                double y0, double w, double h, const uint8_t v[4][4], bool vflip = false) {
    if (w <= 0 || h <= 0)
        return;
    for (int32_t py = 0; py < int32_t(dst.height); ++py) {
        const double cy = py + 0.5;
        if (cy < y0 || cy >= y0 + h)
            continue;
        const double ty = (cy - y0) / h;
        for (int32_t px = 0; px < int32_t(dst.width); ++px) {
            const double cx = px + 0.5;
            if (cx < x0 || cx >= x0 + w)
                continue;
            const double tx = (cx - x0) / w;
            uint8_t s[4];
            Sample(tex, r[0] + tx * r[2], r[1] + (vflip ? 1.0 - ty : ty) * r[3], s);
            double c[4];
            for (int k = 0; k < 4; ++k) {
                const double top = v[0][k] * (1.0 - tx) + v[1][k] * tx;
                const double bot = v[2][k] * (1.0 - tx) + v[3][k] * tx;
                c[k] = s[k] * (top * (1.0 - ty) + bot * ty) / 255.0;
            }
            uint8_t* d = dst.rgba.data() + (size_t(py) * dst.width + px) * 4;
            const double sa = c[3] / 255.0, da = d[3] / 255.0;
            const double oa = sa + da * (1.0 - sa);
            if (oa <= 0.0) {
                d[0] = d[1] = d[2] = d[3] = 0;
                continue;
            }
            for (int k = 0; k < 3; ++k)
                d[k] = uint8_t(std::floor((c[k] * sa + d[k] * da * (1.0 - sa)) / oa + 0.5));
            d[3] = uint8_t(std::floor(oa * 255.0 + 0.5));
        }
    }
}

} // namespace

// fe3h:gauge/<mode 0..3>/<value>/<max>: the activity-point gauge (0x322820) at rest, 400 x 128,
// without the number: track = sprite 0x726 over (37, 14) 351 x 8 with vertex colours top
// 0xFF393939 / bottom 0xFF303030; fill = sprite 0x726 over (37, 14) (351 * value / max) x 8 with
// the mode's left -> right colours (GaugeFill); frame SpriteGaugeFrame[mode] 1:1 at (0, 0).
// value is clamped to 0..max, max >= 1 (both <= 9999).
bool LibraryImpl::Gauge(const EdenDsmodHostApi* host, std::string_view key, fe3h_g1t::Image& out) {
    const auto part = Split(key.substr(11));
    uint32_t mode, value, mx;
    if (part.size() != 3 || !ParseU32(part[0], mode) || !ParseU32(part[1], value) ||
        !ParseU32(part[2], mx) || mode > 3 || value > 9999 || mx > 9999)
        return false;
    mx = std::max<uint32_t>(mx, 1);
    value = std::min(value, mx);
    fe3h_g1t::Image bar_tex, frame_tex;
    uint32_t br[4], fr[4];
    if (!SpriteSource(host, SpriteGaugeBar, bar_tex, br) ||
        !SpriteSource(host, SpriteGaugeFrame[mode], frame_tex, fr) || fr[2] != 400 || fr[3] != 128)
        return false;
    out = {400, 128, std::vector<uint8_t>(size_t(400) * 128 * 4, 0)};
    auto rgba = [](uint32_t c, uint8_t o[4]) {
        o[0] = uint8_t(c >> 24);
        o[1] = uint8_t(c >> 16);
        o[2] = uint8_t(c >> 8);
        o[3] = uint8_t(c);
    };
    uint8_t track[4][4], fill[4][4];
    rgba(0x393939FF, track[0]);
    rgba(0x393939FF, track[1]);
    rgba(0x303030FF, track[2]);
    rgba(0x303030FF, track[3]);
    rgba(GaugeFill[mode][0], fill[0]);
    rgba(GaugeFill[mode][1], fill[1]);
    rgba(GaugeFill[mode][0], fill[2]);
    rgba(GaugeFill[mode][1], fill[3]);
    ColourQuad(out, bar_tex, br, 37.0, 14.0, 351.0, 8.0, track);
    const float wf = 351.0f * float(value) / float(mx); // s0 * value / max in single precision
    ColourQuad(out, bar_tex, br, 37.0, 14.0, double(wf), 8.0, fill);
    const fe3h_g1t::Image frame = CropRect(frame_tex, fr);
    Draw(out, frame, 0, 0, 400, 128, nullptr, 0, 0);
    return true;
}

// fe3h:gbrange/<g>: the battalion gambit area grid of the unit Details page 3/3, 0x5D3CD0(g, 1)
// (called from 0x1132E0 at panel + (56, 268)). g = battalion record +0x27 = row of fixed_data
// s8 (80 x 0xD, [GOT 0x1AA81C0]) and s3 (80 x 0x18, [GOT 0x1AA8610]). Area type t = s8 +0xA;
// target T = (10, 3), user U = (10, 3) on a 22 x 6 board of 24 x 24 cells (pitch static
// main+0x21D9C10 = 24.0): t in {2, 3, 7, 8, 12} -> T = (10, 2); t in {0, 1, 5} -> arrow not
// flipped and the user marker red 0x69D unless s3 +4 = k is in mask 0x2DB96 (k <= 0x11) or 0x12;
// t == 6 -> U = (10, 4). Cells = the shape generator 0x41E4E0(t, T, U, dir 1) (emulated; table
// below, relative to T; t == 3 also T itself, added by 0x5D3CD0). Draws, white, source-over, UV
// = rect / texture (bilinear at texel centres, see ColourQuad): background 0x696 (530 x 146) at
// (0, 0) 531 x 147; cell 0x690 at 24 * (x, y) 24 x 24; user marker 0x69C / 0x69D (64 x 64) and
// the arrow 0x697 (96 x 96, V-flipped unless t in {0, 1, 5}) at 24 * U, 24 x 24. Canvas 531 x 147.
// Types 9..11, 13 are in the table for completeness (unused by the data); 14 (whole map) and
// > 14 are refused.
bool LibraryImpl::GambitRange(const EdenDsmodHostApi* host, std::string_view key,
                              fe3h_g1t::Image& out) {
    uint32_t g;
    if (!ParseU32(key.substr(13), g) || g >= 80)
        return false;
    uint32_t t, k;
    {
        std::scoped_lock lock{io_};
        Table s8, s3;
        if (!Ready(host) || !Weapons() || !Section(fixeddata_, 8, 0xB, s8) ||
            !Section(fixeddata_, 3, 5, s3) || !s8.Row(g) || !s3.Row(g))
            return false;
        t = s8.Row(g)[0xA];
        k = s3.Row(g)[4];
    }
    struct Cell {
        int8_t x, y;
    };
    static const std::vector<Cell> Shapes[14] = {
        {},
        {{0, -1}, {-1, 0}, {1, 0}, {0, 1}},
        {{0, -2}, {-1, -1}, {0, -1}, {1, -1}, {-2, 0}, {-1, 0}, {1, 0}, {2, 0}},
        {{0, -1}, {0, 0}},
        {{2, -1}, {-1, -2}, {0, -2}, {1, -2}, {2, -2}, {-1, -1}, {0, -1}, {1, -1}},
        {{0, -2}, {-1, -1}, {0, -1}, {1, -1}, {-2, 0}, {-1, 0}, {1, 0}, {2, 0}, {-1, 1}, {0, 1},
         {1, 1}, {0, 2}},
        {{0, -3}, {0, 0}, {0, -1}, {0, -2}},
        {{1, 0}, {-1, 0}, {0, 0}},
        {{2, -1}, {-2, 0}, {-1, 0}, {0, 0}, {1, 0}, {2, 0}, {-2, -1}, {-1, -1}, {0, -1}, {1, -1}},
        [] {
            std::vector<Cell> v;
            for (int y = -3; y <= 0; ++y)
                for (int x = -3; x <= 4; ++x)
                    if (!(x == 4 && y == 0))
                        v.push_back({int8_t(x), int8_t(y)});
            v.push_back({4, 0});
            return v;
        }(),
        [] {
            std::vector<Cell> v;
            for (int y = -2; y <= 0; ++y)
                for (int x = -2; x <= 3; ++x)
                    v.push_back({int8_t(x), int8_t(y)});
            return v;
        }(),
        [] {
            std::vector<Cell> v;
            for (int y = -2; y <= 1; ++y)
                for (int x = -2; x <= 4; ++x)
                    if (!(x == 4 && y == 1))
                        v.push_back({int8_t(x), int8_t(y)});
            v.push_back({4, 1});
            return v;
        }(),
        {{1, -1}, {-1, 0}, {0, 0}, {1, 0}, {-1, -1}, {0, -1}},
        {{0, -3}, {0, -1}, {0, -2}},
    };
    if (t >= 14)
        return false;
    int tx = 10, ty = 3, ux = 10, uy = 3;
    uint32_t user = SpriteGambitUserBlue;
    bool flip = true;
    if (t <= 12 && ((1u << t) & 0x118C)) {
        ty = 2;
    } else if (t <= 12 && ((1u << t) & 0x23)) {
        flip = false;
        if (!((k <= 0x11 && ((1u << k) & 0x2DB96)) || k == 0x12))
            user = SpriteGambitUserRed;
    } else if (t == 6) {
        uy = 4;
    }
    fe3h_g1t::Image bg, cell, mark, arrow;
    uint32_t br[4], cr[4], mr[4], ar[4];
    if (!SpriteSource(host, SpriteGambitGrid, bg, br) ||
        !SpriteSource(host, SpriteGambitCell, cell, cr) || !SpriteSource(host, user, mark, mr) ||
        !SpriteSource(host, SpriteGambitArrow, arrow, ar))
        return false;
    static constexpr uint8_t White[4][4] = {
        {255, 255, 255, 255}, {255, 255, 255, 255}, {255, 255, 255, 255}, {255, 255, 255, 255}};
    out = {GambitGridW, GambitGridH, std::vector<uint8_t>(size_t(GambitGridW) * GambitGridH * 4, 0)};
    ColourQuad(out, bg, br, 0.0, 0.0, 531.0, 147.0, White);
    for (const Cell& c : Shapes[t]) {
        int x = tx + c.x, y = ty + c.y;
        if (x < 0 || x > 31 || y < 0 || y > 31)
            x = y = 0; // 0xAE740: out-of-map coordinates collapse to cell (0, 0)
        ColourQuad(out, cell, cr, 24.0 * x, 24.0 * y, 24.0, 24.0, White);
    }
    ColourQuad(out, mark, mr, 24.0 * ux, 24.0 * uy, 24.0, 24.0, White);
    ColourQuad(out, arrow, ar, 24.0 * ux, 24.0 * uy, 24.0, 24.0, White, flip);
    return true;
}

namespace {

constexpr int64_t IconNone = -1;

int64_t IconBody(int64_t i, int64_t v) {
    return (i >= 0xAE && i <= 0xB3 ? 0x3E117 : 0x1F012) + 4 * i + v;
}

// 0x6A3650 (head of a composed icon).
int64_t IconHead(int64_t f, int64_t v, bool p2) {
    if (f < 0x23 || (f >= 0x1F4 && f <= 0x1FB)) {
        if (f >= 0x1F9)
            return 0x3E12F + (f == 0x1FB ? 0x261 : 0x260);
        if (f >= 0x1F4 && f <= 0x1F7)
            return 0x3E12F + f + (p2 ? 0xA3 : 0x9F);
        if (f == 0x22)
            return p2 ? 0x1F44F : 0x1F2FC;
        return (p2 && (f < 0x1A || (f & ~3) == 0x1F4)) ? 0x1F435 + f : 0x1F2DA + f;
    }
    if (f == 0xC7 || (f >= 0x64 && f <= 0xB3))
        return (f >= 0xAE && f <= 0xB3 ? 0x3E12F : 0x1F16D) + 4 * f + v;
    return IconNone;
}

// 0x6A3480 (jump table main+0xD51A84).
int64_t IconMount(int64_t i, int64_t v) {
    static constexpr std::pair<int, int64_t> Mount[] = {
        {116, 0x1F450}, {117, 0x1F454}, {135, 0x1F458}, {136, 0x1F45C}, {137, 0x1F460},
        {138, 0x1F464}, {139, 0x1F468}, {150, 0x1F46C}, {151, 0x1F470}, {152, 0x1F474},
        {155, 0x1F478}, {156, 0x1F47C}, {157, 0x1F480}, {158, 0x1F484}, {159, 0x1F488},
        {160, 0x1F48C}, {161, 0x1F490}, {162, 0x1F494}, {178, 0x3E3FF}, {179, 0x3E403}};
    for (const auto& [k, id] : Mount)
        if (k == i)
            return id + v;
    return IconNone;
}

// 0x6A3220 (whole-unit sprite; NPC jump table main+0xD51980).
int64_t IconWhole(int64_t f, int64_t v, int64_t k, bool p2) {
    if (k > 3 || k < 0)
        k = 0;
    if (f < 0x23 || (f >= 0x1F4 && f <= 0x1FB)) {
        if (f >= 0x1F4) {
            if (f >= 0x1F9)
                return (f == 0x1FB ? 0x3E387 : 0x3E37F) + 4 * (k > 1 ? 0 : k) + v;
            if ((f & ~3) == 0x1F4)
                return (p2 ? 0x3D402 + 8 * f : 0x3D402 + 4 * f + 0x7C0) + 4 * k + v;
            return IconNone; // face 0x1F8: garbage id in the game (unused)
        }
        if (p2) {
            if (f < 0x1A || f == 0x22)
                return (f == 0x22 ? 0x1F166 : 0x1EFC6 + 16 * f) + 4 * k + v;
            return 0x1EFA5 + 8 * f - 0x130 + 4 * k + v;
        }
        if (f <= 0x19)
            return 0x1EEDD + 4 * f + 4 * k + v;
        return 0x1EFA5 + 8 * f - 0x130 + 4 * k + v;
    }
    static constexpr std::pair<int, int64_t> Npc[] = {
        {0x23, 0x1EF91}, {0x24, 0x1EF95}, {0x25, 0x1EF99}, {0x27, 0x1EF9D}, {0x2A, 0x1EFA1},
        {0x2F, 0x1EFA5}, {0x38, 0x1EFA9}, {0x39, 0x1EFAD}, {0x55, 0x3E3CA}, {0x63, 0x1EFA5}};
    for (const auto& [n, id] : Npc)
        if (n == f)
            return id + v;
    return IconNone;
}

} // namespace

// Class sprite index of classdata s0 +8 + 2 * gender (0x6A2750 / 0x6A2BE0 / 0x6A2EA0).
int64_t LibraryImpl::IconClassSprite(uint32_t cls, uint32_t gender) {
    if (cls == 0x5A || cls > 0x63 || gender > 1)
        return IconNone;
    const uint8_t* c = cls_.Row(cls);
    if (!c)
        c = cls_.Row(0);
    if (!c)
        return IconNone;
    const int32_t s = I16(c + 8 + gender * 2);
    return uint16_t(s) <= 0x257 ? s : IconNone;
}

// The class icon (small map-unit sprite) 0x5CF7E0(cls, gender, face, part2, pid, army) queues and
// the render-cache consumer 0x6A2240 draws into a 32 x 32 cell: ordered layers of group-11 sprites
// (1:1, source-over). v = army variant {2, 1, 3, 2}[army - 1] (army 1..4, main+0xCB99A0), else 0.
// Reference + rule text: packaging/fe3h/assets-test/clsicon_layers.py (checked against 0x6A2240 in
// Unicorn on 360 inputs). Returns false for army 4 (the cell is skipped).
bool LibraryImpl::ClassIconLayers(uint32_t cls, uint32_t gender, uint32_t face, bool p2,
                                  uint32_t army, uint32_t pid, bool route_alt,
                                  std::vector<IconLayer>& out) {
    out.clear();
    if (army == 4)
        return false;
    const int64_t v = army >= 1 && army <= 4 ? std::array<int64_t, 4>{2, 1, 3, 2}[army - 1] : 0;
    auto civilian = [&] { // 0x6A37B0
        int64_t a = IconNone, b = IconNone;
        if (cls == 1)
            a = gender == 0 ? 0x1F2D2 : 0x1F2D6, b = gender == 0 ? 0x1F42D : 0x1F431;
        else if (cls == 0)
            a = gender == 0 ? 0x1F2CA : 0x1F2CE, b = gender == 0 ? 0x1F425 : 0x1F429;
        out.push_back({a + v, 0, 0});
        out.push_back({b + v, 6, 2});
    };
    auto parts = [&](int64_t i) {
        const int64_t m = IconMount(i, v);
        const int32_t dx = (i == 0xA3 || i == 0x95) ? 0 : 6;
        int32_t dy = (i == 0xA3 || i == 0x95) ? 0 : 2;
        if (m >= 1) {
            out.push_back({m, 0, 0});
            dy = -4;
        }
        out.push_back({IconBody(i, v), 0, 0});
        out.push_back({IconHead(i, v, p2), dx, dy});
    };
    auto generic = [&] { // 0x6A2BE0
        int64_t s = IconClassSprite(cls, gender);
        if (face == 0x33)
            s = 0x6F;
        if (cls <= 1)
            return civilian();
        const int64_t i = (s < 0 || s > 0x257) ? int64_t(face) : s;
        if ((i >= 0x64 && i < 0xB4) || i == 0xC7)
            parts(i);
    };
    static constexpr uint64_t LordMask = (1ull << 6) | (1ull << 17) | (1ull << 40) |
                                         (1ull << 42) | (1ull << 44) | (1ull << 56) |
                                         (1ull << 57) | (1ull << 58);
    if (face != 0x33 && cls <= 0x3A && ((LordMask >> cls) & 1)) { // 0x6A2560
        constexpr int64_t B = 0x1F18A;
        int64_t w = IconNone;
        if (cls == 0x2A)
            w = gender == 0 ? 0x1F176 : 0x1F17A;
        else if (cls == 0x28)
            w = 0x1F17E;
        else if (cls == 0x2C)
            w = 0x1F182;
        else if (cls == 0x11)
            w = 0x1F186;
        else if (cls == 0x38)
            w = p2 ? B - 0x1A0 : B;
        else if (cls == 0x39)
            w = p2 ? (route_alt ? 0x1F49E : B - 0x190) : B + 4;
        else if (cls == 0x3A)
            w = p2 ? B - 0x180 : B + 8;
        else if (cls == 6 && p2)
            w = face == 0 ? B - 0x1C0 : face == 1 ? B - 0x1B0 : face == 2 ? B + 0xC
                : face == 3 ? (route_alt ? 0x1F49A : B + 0x10) : face == 4 ? B + 0x14 : IconNone;
        else if (cls == 6)
            w = face == 0 ? B - 0x2AD : face == 1 ? B - 0x2A9 : face == 2 ? B
                : face == 3 ? B + 4 : face == 4 ? B + 8 : IconNone;
        if (w >= 0)
            out.push_back({w + v, 0, 0});
    } else if (face < 0x23 || (face >= 0x1F4 && face <= 0x1FB)) { // 0x6A2750
        int64_t k = 0;
        bool use_whole = false;
        if (pid <= 0x4B0) {
            const uint8_t* pr = s0_.Row(pid);
            if (!pr)
                pr = s0_.Row(0);
            if (!pr)
                return false;
            const uint8_t* x9 = nullptr;
            if (pid < 0x25) {
                x9 = p2s3_.Row(pid);
            } else if (pid >= 0x410 && pid <= 0x416) {
                x9 = pr[0x20] != 0xFF ? p2s3_.Row(pr[0x20]) : nullptr;
                if (!x9)
                    x9 = p2s3_.Row(0);
            }
            const bool special =
                p2 && (face < 0x1A || face == 0x22 || (face & ~3u) == 0x1F4);
            if (cls < 2) {
                use_whole = true;
            } else if (special) {
                if (x9 && cls == x9[0x1F])
                    k = 1;
                else if (x9 && cls == x9[0x20])
                    k = 2;
                else if (x9 && cls == x9[0x21])
                    k = 3;
            } else {
                k = cls == pr[0x1A] ? 1 : 0;
            }
            if (!use_whole && k)
                use_whole = pid == 0x1C ? false : pid == 0x22 ? p2 : true;
        }
        const int64_t s = use_whole ? IconNone : IconClassSprite(cls, gender);
        if (use_whole || s < 0) {
            out.push_back({IconWhole(face, v, k, p2), 0, 0});
        } else {
            const int64_t m = IconMount(s, v);
            const int32_t dy = m > 0 ? -4 : 2;
            if (p2 && (face == 0x0B || face == 0x18))
                out.push_back({face == 0x0B ? 0x1F498 : 0x1F499, 6, dy});
            out.push_back({IconBody(s, v), 0, 0});
            out.push_back({IconHead(face, v, p2), 6, dy});
            if (m >= 1)
                out.push_back({m, 0, 0});
        }
    } else if (face <= 0x63) {
        const int64_t w = IconWhole(face, v, 0, false);
        if (w >= 1)
            out.push_back({w, 0, 0});
        else
            generic();
    } else if (face <= 0xB3) {
        generic();
    } else if (face >= 0x122 && face <= 0x13A) { // 0x6A2EA0
        const int64_t s = IconClassSprite(cls, gender);
        if (cls <= 1)
            civilian();
        else if ((s >= 0x64 && s < 0xB4) || s == 0xC7 || s == 0xAD)
            parts(s);
    } else if (face >= 0xFA && face <= 0x10F) { // 0x6A3150
        out.push_back({face == 0x10F ? 0x3E3CE : int64_t(face) + 0x1EEB7, 0, 0});
    }
    return true;
}

// fe3h:clsicon/<cls>/<gender>/<face>/<part2 0|1>/<army>/<pid>[/<route_alt 0|1>]: the 32 x 32 class
// icon cell (see ClassIconLayers; face = person +0x18 asset, gender = map unit +0x104 or person
// +0x26, army = map unit +0xF1 (0 off-map), part2 = save +0x2449F == 1, route_alt = save +0x2449E
// == 3 && part2). Fails (so the caller can fall back) for army 4, an empty icon, any layer outside
// the group-11 atlas (DLC groups 12 / 13 = LINKDATA 34845 / 34846 are not decoded) and for the
// greyed card overlay (w6, not modelled). On screen the cell is point-sampled to 64 x 64 at 1080p.
bool LibraryImpl::ClassIcon(const EdenDsmodHostApi* host, std::string_view key,
                            fe3h_g1t::Image& out) {
    const auto part = Split(key.substr(13));
    uint32_t v[7]{};
    if (part.size() != 6 && part.size() != 7)
        return false;
    for (size_t i = 0; i < part.size(); ++i)
        if (!ParseU32(part[i], v[i]))
            return false;
    if (v[3] > 1 || v[6] > 1)
        return false;
    std::vector<IconLayer> layers;
    {
        std::scoped_lock lock{io_};
        if (!Ready(host) || !Persons() || !Classes() ||
            !ClassIconLayers(v[0], v[1], v[2], v[3] != 0, v[4], v[5], v[6] != 0, layers))
            return false;
    }
    if (layers.empty())
        return false;
    out = {32, 32, std::vector<uint8_t>(32 * 32 * 4, 0)};
    for (const IconLayer& l : layers) {
        if (l.id < 0x1EEDD || l.id > 0x1F4A1)
            return false;
        fe3h_g1t::Image tex;
        uint32_t r[4];
        if (!SpriteSource(host, uint32_t(l.id), tex, r))
            return false;
        const fe3h_g1t::Image s = CropRect(tex, r);
        Draw(out, s, l.dx, l.dy, s.width, s.height, nullptr, 0, 0);
    }
    return true;
}

bool LibraryImpl::DecodeFont(const EdenDsmodHostApi* host, std::span<const uint8_t> bytes,
                             FontOut& out) {
    fe3h_font::Face face;
    if (!host || !fe3h_font::ParseRequest(bytes, face))
        return false;
    fe3h_font::GameTables tables;
    if (!fe3h_font::ReadGameTables(*host, tables))
        return false;
    std::vector<uint8_t> table;
    {
        std::scoped_lock lock{io_};
        const SourcePtr e = Ready(host) ? link_->Entry(fe3h_font::CodeTableEntry) : nullptr;
        if (!e || !e->ReadAll(table, uint64_t{1} << 20))
            return false;
    }
    fe3h_font::Font font;
    if (!fe3h_font::Build(face, tables, fe3h_font::DecodeCodeTable(table), font))
        return false;
    out.line_height = font.line_height;
    out.first_codepoint = font.first_codepoint;
    out.glyphs.resize(font.glyphs.size());
    for (size_t i = 0; i < font.glyphs.size(); ++i) {
        const auto& g = font.glyphs[i];
        out.glyphs[i] = {g.x, g.y, g.w, g.h, g.bearing_x, g.bearing_y, g.advance};
    }
    return true;
}

Library::Library(size_t image_budget) : impl_(std::make_unique<LibraryImpl>(image_budget)) {}
Library::~Library() = default;

std::unique_ptr<Library> CreateLibrary(size_t image_budget) {
    return std::make_unique<Library>(image_budget);
}

bool LoadImage(Library& lib, const EdenDsmodHostApi* host, const char* key,
               std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
    if (!key || strnlen(key, 257) > 256)
        return false;
    return lib.Impl().Load(host, key, rgba, w, h);
}

bool FaceIndex(Library& lib, const EdenDsmodHostApi* host, uint32_t pid, int32_t expr, bool part2,
               uint32_t& index) {
    return lib.Impl().Face(host, pid, expr, part2, index);
}

bool UnitFaceRow(Library& lib, const EdenDsmodHostApi* host, const UnitFaceInput& in,
                 uint32_t& row) {
    return lib.Impl().UnitRow(host, in, row);
}

bool UnitFaceIndex(Library& lib, const EdenDsmodHostApi* host, const UnitFaceInput& in,
                   int32_t expr, uint32_t& index) {
    return lib.Impl().UnitFace(host, in, expr, index);
}

int FacilityMarkerSprite(uint32_t cid) {
    // 0x5D9290: cid 0x12C..0x18C through the jump table at main+0xD4FDBC (0 = the default 0x79E;
    // ids >= 0x7B54 belong to an atlas slot not served here -> -1), cid 0x30 / 0x416 -> 0x7B0.
    static constexpr uint16_t Table[0x61] = {
        0x7A1, 0x7A0, 0x7A3, 0,     0x7C6, 0x7AB, 0x7A7, 0x7A2, 0,     0x79F, 0x7A5, 0,
        0x7B4, 0x7B1, 0x7B2, 0x7B3, 0x7AD, 0x7AE, 0x79D, 0,     0,     0,     0,     0,
        0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
        0,     0,     0,     0,     0,     0,     0,     0x7AF, 0,     0,     0,     0x7B8,
        0x7B8, 0x7B8, 0x7B8, 0,     0,     0,     0,     0,     0,     0,     0,     0,
        0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
        0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
        0,     0,     0,     0,     0,     0,     0,     0,     0x7B0, 0x6FA, 0xFFFF, 0xFFFF,
        0xFFFF};
    if (cid == 0x30 || cid == 0x416)
        return 0x7B0;
    if (cid >= 0x12C && cid <= 0x18C) {
        const uint16_t v = Table[cid - 0x12C];
        return v == 0xFFFF ? -1 : v ? v : 0x79E;
    }
    if (cid == 0x2AF || (cid >= 0x7FF8 && cid <= 0x7FFA))
        return -1; // 0x7B68 / 0x7B6D / 0x5D9438: another slot
    return 0x79E;
}

std::string SportKey(int army, const std::string& face_key, bool small) {
    std::string_view f{face_key};
    if (f.starts_with("module:"))
        f.remove_prefix(7);
    if (f.starts_with("fe3h:"))
        f.remove_prefix(5);
    return std::string(small ? "module:fe3h:sport_s/" : "module:fe3h:sport/") +
           (army < 0 ? std::string("grey") : std::to_string(army)) + "/" + std::string(f);
}

std::string OportKey(const std::string& face_key) {
    std::string_view f{face_key};
    if (f.starts_with("module:"))
        f.remove_prefix(7);
    if (f.starts_with("fe3h:"))
        f.remove_prefix(5);
    return "module:fe3h:oport/" + std::string(f);
}

std::string GaugeKey(uint32_t mode, uint32_t value, uint32_t max) {
    return "module:fe3h:gauge/" + std::to_string(mode) + "/" + std::to_string(value) + "/" +
           std::to_string(max);
}

std::string ClassIconKey(uint32_t cls, uint32_t gender, uint32_t face, bool part2, uint32_t army,
                         uint32_t pid, bool route_alt) {
    std::string k = "module:fe3h:clsicon";
    for (uint32_t v : {cls, gender, face, uint32_t(part2), army, pid})
        k += "/" + std::to_string(v);
    return route_alt ? k + "/1" : k;
}

std::string AdjportKey(const std::string& face_key) {
    std::string_view f{face_key};
    if (f.starts_with("module:"))
        f.remove_prefix(7);
    if (f.starts_with("fe3h:"))
        f.remove_prefix(5);
    return "module:fe3h:adjport/" + std::string(f);
}

std::string GambitRangeKey(uint32_t g) {
    return "module:fe3h:gbrange/" + std::to_string(g);
}

std::string SpriteKey(uint32_t id) {
    return "module:fe3h:sprite/" + std::to_string(id);
}

std::string StillKey(const UnitFaceInput& in) {
    return "module:fe3h:still/" + std::to_string(in.pid) + "/" + std::to_string(in.cls) + "/" +
           std::to_string(in.gender_byte) + "/" + std::to_string(in.slot < 0 ? 0xFF : in.slot) +
           (in.part2 ? "/2" : "");
}

std::string DportKey(uint32_t army, const std::string& face_key, bool small) {
    std::string_view f{face_key};
    if (f.starts_with("module:"))
        f.remove_prefix(7);
    if (f.starts_with("fe3h:"))
        f.remove_prefix(5);
    return std::string(small ? "module:fe3h:dport_s/" : "module:fe3h:dport/") +
           std::to_string(army) + "/" + std::string(f);
}

int UiWeaponItemIcon(Library& lib, uint32_t item_id, uint32_t uses) {
    return lib.Impl().WeaponIcon(item_id, uses);
}

std::string UiKey(uint32_t id, uint32_t entry) {
    return entry == UiAtlasEngU
               ? "module:fe3h:ui/" + std::to_string(id)
               : "module:fe3h:ui/" + std::to_string(entry) + "/" + std::to_string(id);
}

bool DecodeFont(Library& lib, const EdenDsmodHostApi* host, const uint8_t* bytes, size_t size,
                FontOut& out) {
    if (!bytes || size == 0 || size > 4096)
        return false;
    return lib.Impl().DecodeFont(host, std::span<const uint8_t>{bytes, size}, out);
}

int MinimapIndex(uint32_t stage, bool flag) {
    constexpr int Count = 48; // entries in minimap.bin (0x69BF40 rejects index >= count)
    int idx;
    if (stage >= 0x26 && stage <= 0x3B) {
        if (stage >= 0x2A)
            return -1; // code: index 0, a placeholder -- no overview for these stages
        idx = int(stage) + 6;
    } else if (flag) {
        switch (stage) {
        case 0x07: idx = 0x25; break;
        case 0x0D: idx = 0x26; break;
        case 0x0E: idx = 0x27; break;
        case 0x15: idx = 0x28; break;
        case 0x16: idx = 0x29; break;
        default: idx = stage < uint32_t(Count) ? int(stage) : -1; break;
        }
    } else {
        switch (stage) {
        case 0x06: idx = 0x2A; break;
        case 0x12: idx = 0x2B; break;
        default: idx = stage < uint32_t(Count) ? int(stage) : -1; break;
        }
    }
    return idx >= 0 && idx < Count ? idx : -1;
}

uint32_t FaceSlot(int32_t expr) {
    if (expr < 0 || expr >= 14)
        return 0;
    switch (expr) {
    case 6: return 4;
    case 12: return 3;
    case 13: return 2;
    default: return uint32_t(expr > 6 ? expr - 1 : expr);
    }
}

std::string PortraitKey(uint32_t pid) {
    return "module:fe3h:portrait/" + std::to_string(pid);
}
std::string FaceKey(uint32_t pid, int32_t expr, bool part2) {
    return "module:fe3h:face/" + std::to_string(pid) + "/" + std::to_string(expr < 0 ? 0 : expr) +
           (part2 ? "/2" : "");
}
std::string BmapKey(uint32_t stage, bool flag) {
    return "module:fe3h:bmap/" + std::to_string(stage) + (flag ? "/1" : "");
}

} // namespace fe3h_assets
