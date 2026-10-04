// SPDX-License-Identifier: GPL-3.0-or-later
// AfterbirthPlus.nro 0F659482..., Switch update v524288. Derived from named exported
// methods; see companion/isaac/AFTERBIRTH-RESEARCH.md. No Repentance memory offsets.
bool Reader::Impl::ResolveAfterbirth() {
    resolved = false;
    dom_ok.fill(false);
    main_base = host.main_base;
    main_size = host.main_size;
    const auto entry = Get<u64>(main_base + 0x10CC80);
    if (!entry || dsmod_sdk::InMain(main_base, main_size, *entry, 1) || *entry < 0x2A37A0) {
        why_resolve = "AfterbirthPlus.nro not loaded";
        return false;
    }
    const u64 base = *entry - 0x2A37A0;
    constexpr u8 id[20] = {0x0f, 0x65, 0x94, 0x82, 0x74, 0xce, 0x60, 0xec, 0xa5, 0x9c,
                           0x1e, 0x55, 0xb0, 0x9c, 0x54, 0xd0, 0x60, 0x7f, 0x3c, 0x57};
    std::vector<u8> hdr;
    if ((base & 0xfff) || !Block(base, 0x60, hdr) || std::memcmp(hdr.data() + 0x10, "NRO0", 4) ||
        At<u32>(hdr, 0x18) != 0x821000 || std::memcmp(hdr.data() + 0x40, id, 20)) {
        why_resolve = "AfterbirthPlus.nro size/module id mismatch";
        return false;
    }
    char magic[4];
    const auto gg = Get<u64>(base + 0x81D3F0), gm = Get<u64>(base + 0x81D388);
    if (!Read(base + 0x469058, magic, 4) || std::memcmp(magic, "MOD0", 4) || !gg ||
        *gg != base + 0x826AF0 || !gm || *gm != base + 0x827850) {
        why_resolve = "AfterbirthPlus globals/MOD0 mismatch";
        return false;
    }
    static constexpr HashPin pins[] = {
        {Domain::Core, 0x2006b8, 0x3c0, 0xff588cf2e7574bbcULL,
         "ItemPool::GetPillEffect(ePillColor)"},
        {Domain::Core, 0x137df8, 0x258, 0x77a9020445e6f054ULL,
         "Entity_Player::HasCollectible(eCollectibleType) const"},
        {Domain::Core, 0x18ccac, 0x148, 0xaf1d7b7562c3d8b9ULL,
         "Entity_Player::GetMaxPoketItems() const"},
        {Domain::Core, 0x157c44, 0xf8, 0x9d6e54ecd4b82997ULL,
         "Entity_Player::GetZodiacEffect() const"},
        {Domain::Core, 0x2a31f4, 0x5c, 0x0fb2b9767d09d748ULL,
         "TemporaryEffects::HasEffect(eCollectibleType) const"},
        {Domain::Core, 0x269b64, 0x28, 0xa912ec68f9b154e6ULL,
         "RNG::SetSeed(unsigned int, unsigned int)"},
        {Domain::Core, 0x269b8c, 0x7c, 0x5203a2acef099581ULL, "RNG::Random(unsigned int)"},
        {Domain::Core, 0x19a6dc, 0x58, 0xb96552798e56f86aULL,
         "Entity_Player::AddGoldenHearts(int)"},

        {Domain::Core, 0x2a2424, 0x10, 0xfc935ce2e97452adULL, "StageTransition::IsActive() const"},
        {Domain::Core, 0x2964a0, 0x10, 0xfc935ce2e97452adULL, "RoomTransition::IsActive() const"},
        {Domain::Core, 0x1d81cc, 0xc, 0xc202337ae27e4e98ULL, "GenericPrompt::IsActive() const"},
        {Domain::Core, 0xcd8c, 0x60, 0x786bc05329297ceaULL, "ANM2::IsPlaying(char const*) const"},
        {Domain::Core, 0x1c1470, 0xc8, 0x89a8dcf0ca1a15d1ULL, "Game::IsPaused() const"},
        {Domain::Core, 0x1c44a4, 0x1c8, 0xe85a5437dadb7d54ULL,
         "Game::GetRandomPlayer(KAGE::Math::Vector2&, float)"},
        {Domain::Core, 0x1beb88, 0x27c, 0x82bc521447d6e054ULL, "Game::SaveState(GameState&)"},
        {Domain::Core, 0x1f1ae8, 0x1bb4, 0x286e6a2073a0c3f5ULL, "HUD::Update()"},
        {Domain::Core, 0x1f3dcc, 0x3b30, 0x7f7ebb06cfb9fd70ULL, "HUD::Render()"},
        {Domain::Core, 0x25b304, 0x384, 0x3a40329f3b701f7fULL, "PauseScreen::Show()"},
        {Domain::Core, 0x20eb90, 0x4bc, 0xab18c22fff83e52aULL, "Level::UpdateVisibility()"},
        {Domain::Core, 0x211af8, 0xec, 0x7487f0179abe05b1ULL, "Level::GetCurrentRoomDesc()"},
        {Domain::Core, 0x2491d0, 0x152c, 0x637b46f8e0b4c4e2ULL, "Minimap::Config::render()"},
        {Domain::Core, 0x1392a0, 0x4b4, 0xf7e9ebc9fdedaea1ULL, "Entity_Player::AddCoins(int)"},
        {Domain::Core, 0x142c1c, 0x3c, 0x9a4810313fbc686dULL, "Entity_Player::AddBombs(int)"},
        {Domain::Core, 0x142c58, 0x3c, 0xf5bd6ffa582e2ef2ULL, "Entity_Player::AddKeys(int)"},
        {Domain::Core, 0x1372cc, 0x40c, 0x362b9988a2ede84eULL,
         "Entity_Player::AddMaxHearts(int, bool)"},
        {Domain::Core, 0x1376d8, 0x3b4, 0xf0fa173c9234aa97ULL, "Entity_Player::AddSoulHearts(int)"},
        {Domain::Core, 0x183c88, 0x28, 0xb6f59cfa129ecd8aULL, "Entity_Player::GetCard(int) const"},
        {Domain::Core, 0x187cb0, 0x28, 0x23d7266079395d3aULL, "Entity_Player::GetPill(int) const"},
        {Domain::Core, 0x18ce30, 0x1c, 0xf2cb8e7e91fe1378ULL,
         "Entity_Player::GetTrinket(unsigned int) const"},
        {Domain::Core, 0x14abdc, 0x36c, 0x8fe3e09a7b88d0bdULL,
         "Entity_Player::SetActiveCharge(int)"},
        {Domain::Core, 0x146574, 0x39dc, 0x490d4fde6ca8ded5ULL,
         "Entity_Player::AddCollectible(eCollectibleType, int, bool)"},
        {Domain::Core, 0x1eb804, 0x34, 0xb11edc9601445a1cULL,
         "History::HistoryItem::HistoryItem(eCollectibleType, eLevelStage, eStageType, eRoomType, "
         "ItemPool::eItemPoolType, int)"},
        {Domain::Core, 0x27a798, 0x1c, 0x434b4a111283d3baULL, "Room::AddEntity(Entity*)"},
        {Domain::Core, 0x5ea24, 0x328, 0x0761748d3933e353ULL, "EntityList::Add(Entity*)"},
        {Domain::Core, 0x4ce6c, 0x24, 0x85635d616719544aULL, "Entity::GetFrameCount() const"},
        {Domain::Core, 0x128e74, 0x1ff8, 0x9b3ba6830321cbacULL,
         "Entity_Pickup::Init(eEntityType, unsigned int, unsigned int, unsigned int)"},
        {Domain::Core, 0x12ae6c, 0x27c, 0x835d67182cac85a7ULL, "Entity_Pickup::SetPrice(int)"},
        {Domain::Core, 0x29e2ec, 0xbc, 0x32e937f19cc7cacbULL, "Seeds::Seed2String(unsigned int)"},
        {Domain::Core, 0x29e3d4, 0xec, 0x13f65ec323e5d53eULL, "Seeds::SetStartSeed(unsigned int)"},
    };
    std::vector<u8> bytes;
    for (const auto &pin : pins) {
        if (!Block(base + pin.offset, pin.size, bytes) ||
            dsmod_sdk::Fnv1a64(bytes.data(), bytes.size()) != pin.fnv) {
            why_resolve = std::string{"AfterbirthPlus fingerprint mismatch: "} + pin.label;
            return false;
        }
    }
    nro = base;
    if (!Read(base + 0x627396, alphabet, 32)) {
        why_resolve = "AfterbirthPlus seed alphabet unreadable";
        return false;
    }
    alphabet[32] = 0;
    bad_domains.clear();
    dom_ok.fill(true);
    resolved = true;
    why_resolve.clear();
    return true;
}

void Reader::Impl::SampleAfterbirth(Out &o, bool &torn) {
    const auto fail = [&](const char *why) {
        o.I("run.ready", 0);
        o.T("run.why", why);
        in_run = false;
    };
    constexpr u64 GGrid = 0x8A18, GRooms = 0x18, DStride = 0x100, DGrid = 0, DData = 0x10,
                  DDisplay = 0x48, DVisited = 0x4C, DFlags = 0x50;
    constexpr u64 PBlock = 0x2684, PBlockEnd = 0x3A2C, OCounts = 0x29B8, OTrinket = 0x2998;
    constexpr int MaxRoomsList = 138;
    torn = false;
    std::vector<u8> gb, room_state, pb, clock, ph;
    if (!Block(G, 0x10, gb) || !Block(G + 0x8CF8, 0x10, room_state) ||
        !Block(G + 0x277930, 12, clock) || !Block(G + 0xC360, 24, ph)) {
        fail("Afterbirth+ game unreadable");
        return;
    }
    const u64 begin = At<u64>(ph, 0), end = At<u64>(ph, 8);
    const auto player = begin && end > begin && (end - begin) % 8 == 0 && end - begin <= 16 * 8
                            ? Get<u64>(begin)
                            : std::nullopt;
    if (!player || !*player || !Block(*player + PBlock, PBlockEnd - PBlock, pb)) {
        fail("Afterbirth+ no player");
        return;
    }
    P = *player;
    const s32 stage = At<s32>(gb, 0), stype = At<s32>(gb, 4), cur = At<s32>(room_state, 8), dim = 0;
    const u32 curses = At<u32>(gb, 0xC);
    const bool lost = (curses & 4) != 0;
    const s32 frame0 = At<s32>(clock, 4), timer = At<s32>(clock, 8);
    const u64 room_ptr = At<u64>(room_state, 0);
    const auto difficulty_value = Get<s32>(M + 0x3600), challenge_value = Get<s32>(G + 0x297B8C);
    if (!difficulty_value || !challenge_value) {
        fail("Afterbirth+ mode unreadable");
        return;
    }
    const int difficulty = *difficulty_value, challenge = *challenge_value;
    const auto seed = Get<u32>(G + 0xC3DC);
    const auto special = Get<u64>(G + 0xC430);
    if (!seed || !special) {
        fail("Afterbirth+ seeds unreadable");
        return;
    }
    ObserveFloor(*seed, stage, stype);
    const auto pi = [&](u64 off) { return At<s32>(pb, off - PBlock); };
    const auto pf = [&](u64 off) { return At<float>(pb, off - PBlock); };
    // The AB+ helper's ordinary ownership path, including the temporary active (488).
    const auto has = [&](int id) -> std::optional<bool> {
        if (pi(0x28DC) == id || pi(0x3A18) == id)
            return true;
        const u64 counts = At<u64>(pb, 0x29B8 - PBlock), counts_end = At<u64>(pb, 0x29C0 - PBlock);
        if (!counts || counts_end < counts || (counts_end - counts) % 4 ||
            counts_end - counts > 0x4000 || id < 0 ||
            static_cast<u64>(id) >= ((counts_end - counts) / 4))
            return std::nullopt;
        const auto count = Get<s32>(counts + static_cast<u64>(id) * 4);
        if (!count)
            return std::nullopt;
        if (*count > 0)
            return true;
        const u64 begin = At<u64>(pb, 0x2860 - PBlock), end = At<u64>(pb, 0x2868 - PBlock);
        if (end < begin || (end - begin) % 16 || (end - begin) > 4096)
            return std::nullopt;
        for (u64 at = begin; at < end; at += 16) {
            const auto cfg = Get<u64>(at);
            if (!cfg || !*cfg)
                return std::nullopt;
            const auto type = Get<s32>(*cfg), effect = Get<s32>(*cfg + 4);
            if (!type || !effect)
                return std::nullopt;
            if ((*type == 1 || *type == 3 || *type == 4) && *effect == 488) {
                if (pi(0x26CC) == id)
                    return true;
            }
        }
        return false;
    };
    const auto pill_effect = [&](int color) -> std::optional<s32> {
        if (color < 1 || color > 13)
            return std::nullopt;
        const auto raw = Get<s32>(G + 0xA31C + color * 4);
        if (!raw || *raw < 0 || *raw > 46)
            return std::nullopt;
        if (challenge == 42)
            return 4;
        // AB+ uses the first player for PHD/Virgo and heart-dependent remaps.
        int effect = *raw;
        const auto phd = has(75), virgo = has(303), zodiac = has(392);
        if (!phd || !virgo || !zodiac)
            return std::nullopt;
        bool good = *phd || *virgo;
        if (*zodiac) {
            if (stage < 0 || stage > 13)
                return std::nullopt;
            const auto floor_seed = Get<u32>(G + 0xC3F0 + stage * 4);
            const auto shift = Get<u64>(nro + 0x81DA48);
            if (!floor_seed || !shift || !*shift)
                return std::nullopt;
            std::vector<u8> shifts;
            if (!Block(*shift + 12 * 12, 12, shifts))
                return std::nullopt;
            u32 state = *floor_seed;
            for (int i = 0; i < 3; ++i) {
                const auto n = At<u32>(shifts, i * 4);
                if (n > 31)
                    return std::nullopt;
                state ^= i == 1 ? state << n : state >> n;
            }
            const auto sign = Get<s32>(nro + 0x6321B0 + (state % 12) * 4);
            if (!sign)
                return std::nullopt;
            good |= *sign == 303;
        }
        if (good && effect >= 1 && effect <= 42) {
            const auto jump = Get<u8>(nro + 0x632B57 + effect - 1);
            if (!jump)
                return std::nullopt;
            const u64 pc = 0x200804 + *jump * 4;
            if (pc != 0x200874) {
                const auto word = Get<u32>(nro + pc);
                if (!word)
                    return std::nullopt;
                const auto insn = detail::Decode(*word, pc);
                if (insn.kind != detail::Insn::MovzW || insn.rd != 8)
                    return std::nullopt;
                const int index = static_cast<int>(insn.imm);
                const auto mapped = Get<s32>(nro + 0x632C18 + index * 4);
                if (!mapped)
                    return std::nullopt;
                effect = *mapped;
            }
        }
        if (effect == 1)
            effect = pi(0x2688) + pi(0x2690) < 3 ? 5 : 1;
        else if (effect == 6) {
            if (pi(0x2684) <= 2 && pi(0x26F8) != 4)
                effect = 7;
            else if (pi(0x2690) <= 2 && pi(0x26F8) == 4)
                effect = 7;
        }
        return effect;
    };

    // Game::IsPaused, including transitions, achievements and the generic prompt.
    // GenericPrompt::IsActive passes an empty animation name to IsPlaying, so only
    // its current-animation pointer and playing flag are needed (no guest strings).
    bool paused = At<s32>(clock, 0) > 0;
    for (u64 off : {0xD628ULL, 0x4EC64ULL, 0xC2E0ULL, 0xA398ULL, 0x193538ULL}) {
        if (paused)
            break;
        const auto state = Get<s32>(G + off);
        if (!state) {
            fail("Afterbirth+ pause state unreadable");
            return;
        }
        paused = *state != 0;
    }
    if (!paused) {
        const auto state = Get<s32>(G + 0xC440);
        if (!state) {
            fail("Afterbirth+ pause state unreadable");
            return;
        }
        paused = *state == 2;
    }
    if (!paused) {
        const auto playing = Get<u8>(G + 0x297CC0 + 0x244);
        if (!playing) {
            fail("Afterbirth+ prompt unreadable");
            return;
        }
        if (*playing) {
            const auto animation = Get<u64>(G + 0x297CC0 + 0x38);
            if (!animation) {
                fail("Afterbirth+ prompt unreadable");
                return;
            }
            paused = *animation != 0;
        }
    }
    o.I("run.ready", 1);
    o.T("run.why", "");
    o.I("p.ready", 1);
    o.I("run.paused", paused);
    o.I("run.map_held", Get<s32>(G + 0x256050).value_or(0) == 1);
    o.I("floor.stage", stage);
    o.I("floor.type", stype);
    o.I("floor.curses", curses);
    o.I("floor.special", *special != 0);
    o.I("floor.lost", lost);
    o.T("floor.name", text && text->Ready() && !*special
                          ? text->FloorName(stage, stype, curses, (difficulty & ~1) == 2, challenge)
                          : std::string{});
    o.T("seed.text", detail::Seed2String(*seed, alphabet));
    o.T("time.text", detail::TimeText(timer, challenge));
    o.I("p.coins", pi(0x26AC));
    o.I("p.bombs", pi(0x26A8));
    o.I("p.keys", pi(0x26A0));
    o.I("p.gkey", At<u8>(pb, 0x26A4 - PBlock) != 0);
    o.I("p.gbomb", At<u8>(pb, 0x26A5 - PBlock) != 0);
    o.I("p.type", pi(0x26F8));
    o.I("p.h.max", pi(0x2684));
    o.I("p.h.red", pi(0x2688));
    o.I("p.h.eternal", pi(0x268C));
    o.I("p.h.soul", pi(0x2690));
    o.I("p.h.black", std::popcount(static_cast<u32>(pi(0x2694))));
    o.I("p.h.golden", pi(0x2C28));
    o.I("p.h.bone", pi(0x3A28));
    o.I("p.h.broken", 0);
    o.I("p.h.rotten", 0);
    o.I("bag.on", 0);
    o.I("bag.held", 0);
    o.I("bag.n", 0);
    o.I("bag.pvon", 0);
    // AB+ HUD::Update copies fire delay as an integer and negates tear falling speed for range.
    const float speed = pf(0x28C4), delay = static_cast<float>(pi(0x27C0)), damage = pf(0x27D0),
                range = -pf(0x27D4), shot = pf(0x27C4), luck = pf(0x28C8);
    const float values[] = {speed, delay, damage, range, shot, luck};
    const float range_tally = (shot - 1.f) / 0.15f + 1.f + pf(0x27D8) * 2.f + range / 20.f;
    const float tallies[] = {speed * 2.f, 15.f / delay, damage / 1.5f,
                             range_tally, shot * 2.f,   luck};
    static constexpr const char *labels[] = {"speed", "tears", "damage", "range", "shot", "luck"};
    for (int i = 0; i < 6; ++i) {
        const auto val = detail::StatText(values[i]);
        o.T(std::string{"st."} + labels[i], val);
        o.I("st." + std::to_string(i) + ".len", val.size());
        o.I("st." + std::to_string(i) + ".tally",
            static_cast<int>(std::clamp(std::isfinite(tallies[i]) ? tallies[i] : 7.f, 1.f, 7.f)));
    }
    o.I("st.ready", 1);
    o.I("st.tally_max", 7);
    std::array<s32, 4> act_id{};
    for (int i = 0; i < 4; ++i) {
        const std::string k = "act." + std::to_string(i) + ".";
        const u64 off = i == 0 ? 0x28DC : 0x3A18;
        const int id = i < 2 ? pi(off) : 0, charge = i < 2 ? pi(off + 4) : 0;
        act_id[i] = id;
        const auto cfg =
            id > 0 && text ? text->Item(isaac_text::Kind::Collectible, id) : isaac_text::ItemInfo{};
        const int max = cfg.valid ? cfg.max_charges : 0;
        o.I(k + "id", id);
        o.I(k + "charge", id ? charge : 0);
        o.I(k + "max", max);
        o.I(k + "battery", i < 2 ? pi(off + 8) : 0);
        o.T(k + "name", id ? Name(isaac_text::Kind::Collectible, id) : std::string{});
        o.T(k + "key", id ? Key("coll", id) : std::string{});
        o.T(k + "bar_key", id && max > 0
                               ? "module:isaac:charge/" + std::to_string(charge) + "/" +
                                     std::to_string(max) + "/" + std::to_string(pi(off + 8))
                               : std::string{});
    }
    for (int i = 0; i < 2; ++i) {
        const int id = pi(OTrinket + i * 4);
        last_tr[i] = id;
        const std::string k = "tr." + std::to_string(i) + ".";
        o.I(k + "id", id);
        o.I(k + "gold", 0);
        o.T(k + "key", id ? Key("trink", id) : std::string{});
        o.T(k + "name", id ? Name(isaac_text::Kind::Trinket, id) : std::string{});
    }
    std::array<std::pair<s32, s32>, 4> pockets{};
    for (int i = 0; i < 4; ++i) {
        const int id = i < 2 ? pi(0x2A98 + i * 8) : 0,
                  kind = id ? At<u8>(pb, 0x2A9C + i * 8 - PBlock) : -1;
        pockets[i] = {kind, id};
        const std::string k = "pk." + std::to_string(i) + ".";
        o.I(k + "id", id);
        o.I(k + "kind", kind);
        o.T(k + "key", id ? "module:isaac:pocket/" + std::to_string(kind) + "/" + std::to_string(id)
                          : std::string{});
    }
    bool two = false, capacity_ok = true;
    for (int id : {251, 252, 416, 454}) {
        const auto own = has(id);
        if (!own)
            capacity_ok = false;
        else
            two |= *own;
    }
    o.I("pk.max", capacity_ok ? (two ? 2 : 1) : 0);
    int held_effect = -1;
    bool identified = false;
    if (pockets[0].first == 0) {
        const int color = pockets[0].second;
        const auto known = color >= 1 && color <= 13 ? Get<u8>(G + 0xA354 + color) : std::nullopt;
        identified = known && *known;
        if (identified)
            held_effect = pill_effect(color).value_or(-1);
    }
    o.I("pk.0.ident", identified);
    o.I("pk.0.effect", held_effect);
    o.T("pk.0.name",
        pockets[0].first == 1 ? Name(isaac_text::Kind::Card, pockets[0].second)
        : pockets[0].first == 0
            ? (held_effect >= 0 ? Name(isaac_text::Kind::PillEffect, held_effect) : "???")
            : std::string{});
    o.T("pk.btn_key", "module:isaac:ui/gfx/ui/buttons.anm2#Switch#11");
    o.I("pk.btn_live", 0);
    o.I("pk.btn_frame", 11);
    last_act = act_id;
    last_pk = pockets;
    // ---- MAP page: rooms of the current dimension the minimap draws ----
    // the current dimension's 13x13 grid (list index per cell): whole on the map page
    std::vector<u8> grid;
    const bool dim_ok = dim >= 0 && dim < 3;
    const bool grid_ok = dim_ok && ui_page == PageMap &&
                         Block(G + GGrid + static_cast<u64>(dim) * 169 * 4, 169 * 4, grid);
    s32 cur_li = -1;
    if (cur >= 0 && cur < 169 && dim_ok) {
        const auto li = grid_ok ? std::optional<s32>{At<s32>(grid, static_cast<u64>(cur) * 4)}
                                : Get<s32>(G + GGrid + (static_cast<u64>(dim) * 169 + cur) * 4);
        if (li && *li >= 0 && *li < MaxRoomsList)
            cur_li = *li;
    } else if (cur <= -1 && cur >= -10) {
        cur_li = 0x7F - cur;
    }
    o.I("map.cur", cur_li);
    if (ui_page == PageMap) {
        std::vector<MapRoom> rooms;
        bool ok = Ok(Domain::Map) && grid_ok;
        std::vector<u8> rb;
        ok = ok && Block(G + GRooms, static_cast<std::size_t>(MaxRoomsList) * DStride, rb);
        if (ok) {
            std::vector<bool> seen(static_cast<std::size_t>(MaxRoomsList), false);
            std::vector<s32> order;
            for (int c = 0; c < 169; ++c) {
                const s32 li = At<s32>(grid, static_cast<u64>(c) * 4);
                if (li >= 0 && li < MaxRoomsList && !seen[static_cast<std::size_t>(li)]) {
                    seen[static_cast<std::size_t>(li)] = true;
                    order.push_back(li);
                }
            }
            std::sort(order.begin(), order.end());
            if (room_data_cache.size() > 2048)
                room_data_cache.clear();
            for (const s32 li : order) {
                const u64 d = static_cast<u64>(li) * DStride;
                const s32 gidx = At<s32>(rb, d + DGrid);
                const u64 data = At<u64>(rb, d + DData);
                const u32 disp = At<u32>(rb, d + DDisplay);
                const s32 visited = At<s32>(rb, d + DVisited);
                const u32 flags = At<u32>(rb, d + DFlags);
                const bool is_cur = li == cur_li;
                // Config::render: outline/fill need Data != 0 and DisplayFlags != 0 (or current)
                if (!data || (!disp && !is_cur) || gidx < 0 || gidx >= 169)
                    continue;
                auto it = room_data_cache.find(data);
                if (it == room_data_cache.end()) {
                    // AB RoomConfig::Room: +0x4 type, +0xC subtype, +0x4C shape (one read)
                    u8 rd[0x4C];
                    if (!Read(data + 0x4, rd, sizeof(rd))) {
                        ok = false;
                        break;
                    }
                    Impl::RoomData v{};
                    std::memcpy(&v.type, rd, 4);
                    std::memcpy(&v.subtype, rd + 0x8, 4);
                    std::memcpy(&v.shape, rd + 0x48, 4);
                    it = room_data_cache.emplace(data, v).first;
                }
                const Impl::RoomData &rd = it->second;
                MapRoom r{};
                r.grid = static_cast<u8>(gidx);
                r.shape = static_cast<u8>(rd.shape);
                r.type = static_cast<u8>(rd.type);
                r.dflags = static_cast<u8>((disp & 7) | (visited > 0 ? 0x80 : 0));
                r.flags = detail::MapFlags(flags, rd.type, rd.subtype);
                r.current = is_cur ? 1 : 0;
                rooms.push_back(r);
            }
        }
        o.T("map.key",
            ok ? "module:isaac:map/" + isaac_mapreg::Register(detail::MapHex(rooms, lost))
               : std::string{});
        o.I("map.rooms", ok && !lost ? static_cast<s64>(rooms.size()) : 0);
        o.I("map.ok", ok ? 1 : 0);
        // the page's default map view (round 3): the image's own layout (isaac_map::PlanLayout,
        // the same function the asset side draws with): zoom MaxScale / k = rooms at the owner's
        // reference size (17x15 cells x6), centred on the current room; zoom 1 = the whole floor
        isaac_map::Layout lay;
        if (ok && !lost) {
            isaac_map::Floor fl;
            for (const MapRoom &r : rooms)
                fl.rooms.push_back({r.grid, r.shape, r.type, r.dflags, r.flags, r.current});
            lay = isaac_map::PlanLayout(fl);
        }
        o.F("map.vzoom", lay.ok ? static_cast<double>(isaac_map::MaxScale) / lay.k : 1.0);
        o.F("map.vcx", lay.ok ? lay.cx : 0.5);
        o.F("map.vcy", lay.ok ? lay.cy : 0.5);
        o.I("map.vsig", lay.ok ? static_cast<s64>(lay.sig) : 0);
        o.I("map.k", lay.ok ? static_cast<s64>(lay.k) : 0);
    }

    // ---- description of one item (ITEMS selection isel.*, ROOM selection info.*) ----
    // text kind (isaac_text::Kind; -1 = nothing), id, image key, name override ("" = game name)
    const auto describe = [&](const std::string &pre, s32 kind, s32 id, std::string key,
                              std::string name_override) {
        std::string name, quote, pools, eid;
        s64 quality = -1;
        if (kind >= 0 && (id > 0 || kind == 3) && text && text->Ready()) {
            const auto info = text->Item(static_cast<isaac_text::Kind>(kind), id);
            if (info.valid) {
                name = info.name;
                quote = info.quote;
                quality = info.quality;
                pools = info.pools;
            }
            eid = text->Eid(static_cast<isaac_text::Kind>(kind), id);
        }
        if (!name_override.empty())
            name = std::move(name_override);
        o.I(pre + "kind", kind);
        o.I(pre + "id", id);
        o.T(pre + "key", std::move(key));
        o.T(pre + "name", std::move(name));
        o.T(pre + "quote", std::move(quote));
        o.I(pre + "quality", quality);
        o.T(pre + "pools", std::move(pools));
        // text shape for the page's scroll box (paragraphs, code points without the breaks)
        s64 paras = eid.empty() ? 0 : 1, chars = 0;
        for (const char ch : eid) {
            if (ch == '\n')
                ++paras;
            else if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80)
                ++chars;
        }
        o.I(pre + "eid_paras", paras);
        o.I(pre + "eid_chars", chars);
        o.I(pre + "eid_on", eid.empty() ? 0 : 1);
        o.T(pre + "eid", std::move(eid));
    };
    u64 hist[2]{};
    if (!Read(G + 0x9A78, hist, sizeof(hist)) || hist[1] < hist[0] || (hist[1] - hist[0]) % 0x18 ||
        hist[1] - hist[0] > 2048 * 0x18) {
        fail("Afterbirth+ history unreadable");
        return;
    }
    // ---- ITEMS page: history, the items selection ----
    const u64 hist_n = (hist[1] - hist[0]) / 0x18;
    std::vector<HistItem> history;
    if (ui_page == PageItems) {
        std::vector<u8> hb;
        if (Block(hist[0], static_cast<std::size_t>(hist_n) * 0x18, hb)) {
            for (u64 i = 0; i < hist_n; ++i)
                history.push_back({At<s32>(hb, i * 0x18), 0, At<s32>(hb, i * 0x18 + 4)});
        }
        last_hist = history;
    }
    if (ui_page == PageItems) {
        // the selection holds while its item is still where it was tapped
        s32 inv_sel = -1;
        if (isrc == ISrc::Inv) {
            const auto it = std::find_if(history.begin(), history.end(), [&](const HistItem &h) {
                return h.frame == isel_frame && h.id == isel_raw &&
                       (h.trinket ? 1 : 0) == isel_kind;
            });
            if (it == history.end())
                isrc = ISrc::None;
            else
                inv_sel = static_cast<s32>(it - history.begin());
        } else if (isrc == ISrc::Act) {
            if (act_id[static_cast<std::size_t>(isel_slot)] != isel_raw || isel_raw <= 0)
                isrc = ISrc::None;
        } else if (isrc == ISrc::Tr) {
            if (last_tr[static_cast<std::size_t>(isel_slot)] != isel_raw || !(isel_raw & 0x7FFF))
                isrc = ISrc::None;
        } else if (isrc == ISrc::Pk) {
            if (pockets[static_cast<std::size_t>(isel_slot)] !=
                    std::pair<s32, s32>{isel_kind, isel_raw} ||
                isel_kind < 0)
                isrc = ISrc::None;
        }
        u64 cv[2];
        std::vector<u8> counts;
        const bool cok = Read(P + OCounts, cv, sizeof(cv)) && cv[1] >= cv[0] &&
                         (cv[1] - cv[0]) <= 0x4000 &&
                         Block(cv[0], static_cast<std::size_t>(cv[1] - cv[0]), counts);
        o.I("inv.count", static_cast<s64>(history.size()));
        for (std::size_t i = 0; i < history.size(); ++i) {
            const std::string k = "inv." + std::to_string(i) + ".";
            const HistItem &h = history[i];
            o.I(k + "id", h.id);
            o.I(k + "trinket", h.trinket ? 1 : 0);
            o.I(k + "sel", static_cast<s32>(i) == inv_sel && isrc == ISrc::Inv ? 1 : 0);
            if (h.trinket) {
                const s32 tid = h.id & 0x7FFF;
                o.T(k + "key", Key("trink", tid) + ((h.id & 0x8000) ? "/g" : ""));
                o.I(k + "n", 1);
            } else {
                o.T(k + "key", Key("coll", h.id));
                o.I(k + "n", cok && h.id >= 0 && static_cast<u64>(h.id) * 4 + 4 <= counts.size()
                                 ? At<s32>(counts, static_cast<u64>(h.id) * 4)
                                 : 0);
            }
        }
        // the selected item's description (shown on the ITEMS page in place of the stats)
        s32 kind = -1, id = 0;
        std::string key, name_override;
        switch (isrc) {
        case ISrc::Inv:
        case ISrc::Tr: {
            const bool trinket = isrc == ISrc::Tr || isel_kind == 1;
            if (trinket) {
                kind = 1;
                id = isel_raw & 0x7FFF;
                key = Key("trink", id) + ((isel_raw & 0x8000) ? "/g" : "");
            } else {
                kind = 0;
                id = isel_raw;
                key = Key("coll", id);
            }
            break;
        }
        case ISrc::Act:
            kind = 0;
            id = isel_raw;
            key = Key("coll", id);
            break;
        case ISrc::Pk: {
            // RenderPocketItems: pill -> effect name when identified, else "#QUESTION_MARKS_NAME";
            // card -> card; pocket active -> the item (art = the HUD pocket icon, 32x32)
            key =
                "module:isaac:pocket/" + std::to_string(isel_kind) + "/" + std::to_string(isel_raw);
            if (isel_kind == 1) {
                kind = 2;
                id = isel_raw;
            } else if (isel_kind == 2) {
                kind = 0;
                id = isel_raw;
                key = Key("coll", id);
            } else if (isel_kind == 0) {
                kind = 3;
                const int color = isel_raw;
                const auto known =
                    color >= 1 && color <= 13 ? Get<u8>(G + 0xA354 + color) : std::nullopt;
                id = known && *known ? pill_effect(color).value_or(-1) : -1;
                if (id < 0)
                    name_override = "???";
            }
            break;
        }
        case ISrc::Item:
            kind = isel_kind;
            id = isel_raw;
            key = kind == 0   ? Key("coll", id)
                  : kind == 1 ? Key("trink", id)
                  : kind == 2 ? "module:isaac:pocket/1/" + std::to_string(id)
                              : std::string{};
            break;
        case ISrc::None:
            break;
        }
        o.I("isel.on", isrc != ISrc::None ? 1 : 0);
        o.I("isel.src", static_cast<s64>(isrc));
        o.I("isel.slot", isrc == ISrc::None ? -1 : isrc == ISrc::Inv ? inv_sel : isel_slot);
        o.I("isel.act", isrc == ISrc::Act ? isel_slot : -1);
        o.I("isel.tr", isrc == ISrc::Tr ? isel_slot : -1);
        o.I("isel.pk", isrc == ISrc::Pk ? isel_slot : -1);
        describe("isel.", kind, id, std::move(key), std::move(name_override));
    }

    if (room_key != (G ^ room_ptr)) {
        room_key = G ^ room_ptr;
        sel_ped = false;
        near_entity = 0;
    }
    // ---- ROOM page: pedestals, the nearest one (EID rule), the selection ----
    if (ui_page == PageRoom) {
        std::vector<Pedestal> peds;
        bool rok = Ok(Domain::Room) && room_ptr;
        const bool near_on = Ok(Domain::Near);
        u64 el[2] = {};
        if (rok) {
            u8 lb[0x10]{};
            rok = Read(room_ptr + 0x15D0 + 0x70, lb, sizeof(lb));
            std::memcpy(&el[0], lb, 8);
            u32 n;
            std::memcpy(&n, lb + 0xC, 4);
            el[1] = n;
            rok = rok && el[1] <= 1024;
        }
        std::vector<u64> ents(static_cast<std::size_t>(rok ? el[1] : 0));
        if (rok && !ents.empty())
            rok = Read(el[0], ents.data(), ents.size() * 8);
        const bool blind_curse = dim != 2 && (curses & 64); // Entity_Pickup::ReloadGraphics
        for (std::size_t i = 0; rok && i < ents.size(); ++i) {
            s32 tvs[3];
            if (!ents[i] || !Read(ents[i] + 0x30, tvs, sizeof(tvs)))
                continue;
            if (tvs[0] != 5 || tvs[1] != 100 || tvs[2] == 0) // collectible, not taken (C4)
                continue;
            const auto price = Get<s32>(ents[i] + 0xC50);
            const auto spawn = Get<s32>(ents[i] + 0x664);
            const auto x = Get<float>(ents[i] + 0x680), y = Get<float>(ents[i] + 0x684),
                       sz = Get<float>(ents[i] + 0x6B4);
            if (!price || !spawn || !x || !y || !sz)
                continue;
            Pedestal pd{};
            pd.entity = ents[i];
            pd.id = tvs[2];
            pd.price = *price;
            pd.opt = 0;
            pd.blind = blind_curse;
            pd.near.flags = 0;
            pd.near.spawn = *spawn;
            pd.near.x = *x;
            pd.near.y = *y;
            pd.near.size = *sz;
            peds.push_back(pd);
        }
        last_peds = peds;
        // EID: the closest describable entity to the player (EID.player = Isaac.GetPlayer(0))
        int near = -1;
        float near_d = -1.0f;
        float pp[2];
        if (near_on && !peds.empty() && Read(P + 0x680, pp, sizeof(pp))) {
            std::vector<detail::NearCand> cands;
            for (const Pedestal &pd : peds)
                cands.push_back(pd.near);
            near = detail::NearestPedestal(pp[0], pp[1], cands, frame0, EidRadius, &near_d);
        }
        const u64 near_ent = near >= 0 ? peds[static_cast<std::size_t>(near)].entity : 0;
        int shown = -1;
        if (sel_ped) {
            // by entity: a pedestal whose item changes in place (D6 reroll, Glitched Crown /
            // Binge Eater cycling) stays the tapped one; a taken item (subtype 0) leaves the list
            const auto it = std::find_if(peds.begin(), peds.end(),
                                         [&](const Pedestal &p) { return p.entity == sel_entity; });
            // gone, or another pedestal became the nearest one (neither the tapped one nor the one
            // that was nearest at the tap; a pedestal whose FrameCount restarts when its item
            // cycles drops out of range for a frame, so "none" in between is not a change)
            if (it == peds.end() || (near_ent && near_ent != sel_entity && near_ent != tap_near))
                sel_ped = false;
            else
                shown = static_cast<int>(it - peds.begin());
        }
        if (near_ent)
            near_entity = near_ent;
        const bool autosel = !sel_ped && near >= 0;
        if (!sel_ped)
            shown = near >= 0 ? near : (peds.empty() ? -1 : 0);

        o.I("ped.count", static_cast<s64>(peds.size()));
        for (std::size_t i = 0; i < peds.size(); ++i) {
            const std::string k = "ped." + std::to_string(i) + ".";
            const Pedestal &pd = peds[i];
            o.I(k + "id", pd.blind ? 0 : pd.id);
            o.T(k + "key", pd.blind ? std::string{} : Key("coll", pd.id));
            o.I(k + "price", pd.price);
            o.I(k + "blind", pd.blind ? 1 : 0);
            o.I(k + "opt", pd.opt);
            o.I(k + "near", static_cast<int>(i) == near ? 1 : 0);
            o.I(k + "sel", static_cast<int>(i) == shown ? 1 : 0);
            o.T(k + "name", pd.blind ? std::string{} : Name(isaac_text::Kind::Collectible, pd.id));
        }
        o.I("room.near", near);
        o.I("room.near_dist", near >= 0 ? static_cast<s64>(std::lround(near_d)) : -1);
        o.I("room.sel", shown);
        o.I("room.auto", autosel ? 1 : 0);
        if (shown >= 0) {
            const Pedestal &pd = peds[static_cast<std::size_t>(shown)];
            o.I("info.blind", pd.blind ? 1 : 0);
            if (pd.blind) // hidden item: the game's "?" art, no text (EID: question mark)
                describe("info.", 0, 0, "module:isaac:coll/q", {});
            else
                describe("info.", 0, pd.id, Key("coll", pd.id), {});
        } else {
            o.I("info.blind", 0);
            describe("info.", -1, 0, {}, {});
        }
    }
    const auto frame1 = Get<s32>(G + 0x277934);
    const auto player1 = Get<u64>(begin);
    std::vector<u8> check, level1, room1, players1;
    u64 hist1[2]{};
    torn = !frame1 || *frame1 != frame0 || !player1 || *player1 != P || !Block(G, 0x10, level1) ||
           level1 != gb || !Block(G + 0x8CF8, 0x10, room1) || room1 != room_state ||
           !Block(G + 0xC360, 24, players1) || players1 != ph ||
           !Read(G + 0x9A78, hist1, sizeof(hist1)) || std::memcmp(hist1, hist, sizeof(hist)) ||
           !Block(P + PBlock, 0x30, check) || !std::equal(check.begin(), check.end(), pb.begin());
}
