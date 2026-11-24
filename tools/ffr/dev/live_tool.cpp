// Runs ffr::Reader on the desktop against the device's FFR_DEV memory server (adb forward tcp:47123).
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#define private public  // dev tool: look at the reader's object slots
#include "../../../native/modules/ffr_reader.h"
#undef private
static int fd = -1;
static bool rx(void* p, size_t n) { char* c = (char*)p; while (n) { ssize_t r = recv(fd, c, n, 0); if (r <= 0) return false; c += r; n -= r; } return true; }
static EdenDsmodBool rd(void*, uint64_t a, void* out, size_t n) {
    uint8_t h[13]; h[0] = 1; memcpy(h + 1, &a, 8); uint32_t l = uint32_t(n); memcpy(h + 9, &l, 4);
    send(fd, h, 13, 0); uint8_t ok = 0; if (!rx(&ok, 1) || !ok) return EDEN_DSMOD_FALSE; return rx(out, n) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}
int main() {
    fd = socket(AF_INET, SOCK_STREAM, 0); sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(47123); sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (sockaddr*)&sa, sizeof sa)) { perror("connect"); return 1; }
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    uint8_t h[13] = {2}; send(fd, h, 13, 0); uint64_t info[5]; rx(info, 40);
    EdenDsmodHostApi host{}; host.main_base = info[0]; host.main_size = info[1]; host.read_memory = rd;
    ffr::Reader r; std::string why;
    if (!r.Init(&host, "84E182F72EE1DA4F", why)) { printf("init: %s\n", why.c_str()); return 1; }
    ffr::GameState s; r.Update(&host, s);
    printf("save %d money %d party %zu units %zu inv %zu equip %zu battle %d status '%s'\n", s.save_ok, s.money, s.party.size(), s.units.size(), s.inventory.size(), s.equipment.size(), s.in_battle, s.status.c_str());
    for (auto& u : s.units) printf(" unit %d lv %d hp %d/%d mp %d/%d\n", u.id, u.level, u.hp, u.max_hp, u.mp, u.max_mp);
    auto& m = s.map; printf("name %s\n", m.name.c_str()); printf("map valid %d id %d area %d p (%.0f,%.0f) yaw %.0f tex '%s' box (%.0f,%.0f %.0fx%.0f)\n", m.valid, m.map_id, m.area_id, m.px, m.py, m.yaw, m.texture.c_str(), m.wx, m.wy, m.bw, m.bh);
    printf("world %d islands %zu icons %zu\n", m.world, m.islands.size(), m.icons.size());
    for (auto& is : m.islands) printf(" isl %d %s (%.0f,%.0f %.0fx%.0f)\n", is.area, is.texture.c_str(), is.wx, is.wy, is.bw, is.bh);
    for (size_t i = 0; i < m.icons.size() && i < 8; ++i) printf(" icon %s (%.0f,%.0f)\n", m.icons[i].icon.c_str(), m.icons[i].x, m.icons[i].y);
    printf("battle mgr %llx vm %llx cmdmgr %llx lists %zu\n", (unsigned long long)r.battle_.addr, (unsigned long long)r.cmd_vm_.addr, (unsigned long long)r.cmd_mgr_.addr, r.cmd_lists_.size());
    for (auto& o : r.cmd_lists_) printf(" list %llx %s\n", (unsigned long long)o.addr, r.ClassName(o.cls).c_str());
    for (auto& b : s.battle) printf(" bu %d enemy %d hp %d/%d unit %llx param %llx\n", b.id, b.enemy, b.hp, b.max_hp, (unsigned long long)b.unit, (unsigned long long)b.param);
    auto& mn = s.menu; printf("menu valid %d open %d state %d cursor %d actor %d entries %zu path %zu targets %zu list %llx\n", mn.valid, mn.open, mn.state, mn.cursor, mn.actor, mn.entries.size(), mn.path.size(), mn.targets.size(), (unsigned long long)mn.list);
    for (auto& e : mn.entries) printf(" entry %d:%d\n", e.cat, e.id);
    {  // AreaManager.AreaDataMap: Map<int, MapAreaData(0x20)>, element 0x30 (Clarity LinearColor +8, AreaBoxList +0x18)
        uint64_t am = r.g_.Get<uint64_t>(r.minimap_.addr + 0x80);
        uint64_t data = r.g_.Get<uint64_t>(am + 0x40); int num = r.g_.Get<int32_t>(am + 0x48);
        printf("areadata %d cur %d\n", num, r.g_.Get<int32_t>(am + 0x90));
        for (int i = 0; i < num && i < 64; ++i) { uint8_t e[0x30]; r.g_.Read(data + 0x30ull * i, e, 0x30); float c[4]; memcpy(c, e + 8, 16); int boxes; memcpy(&boxes, e + 0x20, 4);
            printf(" area %d clarity %.2f %.2f %.2f %.2f boxes %d\n", *(int*)e, c[0], c[1], c[2], c[3], boxes); }
    }
}
