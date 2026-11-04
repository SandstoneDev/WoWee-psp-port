// psp_dbc_conv - client DBC tables for the PSP (factions).
//
//   psp_dbc_conv --extracted <dir> --out <psp/bin/data/dbc.pak> [--verify] [--dump <file.tsv>]
//   psp_dbc_conv --selftest
//
// Reads <extracted>/dbfilesclient/factiontemplate.dbc and faction.dbc with
// upstream's DBCFile (src/pipeline/dbc_loader.cpp) and writes the columns
// upstream's game::buildFactionHostilityMap and
// GameHandler::factionBaseReputation read into the "factions" entry (WFTP,
// include/rendering/psp/psp_dbc_format.hpp). The PSP reads it once at world
// entry, builds the player's race map with the same header's
// buildFactionReactions (= upstream's function) and closes the pak.
//
// --verify   reads the pak back through the PSP's FactionTableView, runs the
//            hostility map for every playable race and checks what any
//            3.3.5a client must say: each race is friendly with its own
//            template, and the other faction's player templates are hostile
//            (Alliance <-> Horde). Exit code 3 on a mismatch.
// --dump     TSV of the baked rows (template: id faction group friend enemy
//            enemies; faction: id repList races classes base flags).
// --selftest synthetic rows (no client data) through the same code: runs
//            anywhere.
#include "pipeline/dbc_loader.hpp"
#include "rendering/psp/psp_dbc_format.hpp"

#include "conv_common.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace wowee;
using namespace wowee::pspconv;
namespace fmt = wowee::rendering::psp;

namespace {

bool loadDbc(const std::string& root, const char* name, pipeline::DBCFile& dbc) {
    std::vector<uint8_t> bytes;
    return readFile(root + "/dbfilesclient/" + name, bytes) && dbc.load(bytes);
}

// Upstream's columns (Data/expansions/wotlk/dbc_layouts.json = buildFactionHostilityMap's
// fallbacks when no layout is active).
constexpr uint32_t kFtId = 0, kFtFaction = 1, kFtGroup = 3, kFtFriend = 4, kFtEnemy = 5, kFtEnemy0 = 6;
constexpr uint32_t kFacId = 0, kFacRepList = 1, kFacRace0 = 2, kFacClass0 = 6, kFacBase0 = 10, kFacFlags0 = 14;

std::vector<uint8_t> buildBlob(const std::vector<fmt::FactionTemplateRow>& t, const std::vector<fmt::FactionRow>& f) {
    std::vector<uint8_t> out(sizeof(fmt::FactionTableHeader) + t.size() * sizeof(fmt::FactionTemplateRow) +
                             f.size() * sizeof(fmt::FactionRow));
    fmt::FactionTableHeader h{};
    std::memcpy(h.magic, "WFTP", 4);
    h.version = fmt::kFactionTableVersion;
    h.templateCount = static_cast<uint32_t>(t.size());
    h.factionCount = static_cast<uint32_t>(f.size());
    uint8_t* p = out.data();
    std::memcpy(p, &h, sizeof(h));
    p += sizeof(h);
    if (!t.empty()) std::memcpy(p, t.data(), t.size() * sizeof(t[0]));
    p += t.size() * sizeof(fmt::FactionTemplateRow);
    if (!f.empty()) std::memcpy(p, f.data(), f.size() * sizeof(f[0]));
    return out;
}

bool readTables(const std::string& root, std::vector<fmt::FactionTemplateRow>& tOut,
                std::vector<fmt::FactionRow>& fOut) {
    pipeline::DBCFile ft, fac;
    if (!loadDbc(root, "factiontemplate.dbc", ft)) {
        std::fprintf(stderr, "missing %s/dbfilesclient/factiontemplate.dbc\n", root.c_str());
        return false;
    }
    if (!loadDbc(root, "faction.dbc", fac)) {
        std::fprintf(stderr, "missing %s/dbfilesclient/faction.dbc\n", root.c_str());
        return false;
    }
    if (ft.getFieldCount() <= kFtEnemy0 + 3 || fac.getFieldCount() <= kFacFlags0 + 3) {
        std::fprintf(stderr, "unexpected field counts: FactionTemplate %u, Faction %u (want 3.3.5a)\n",
                     ft.getFieldCount(), fac.getFieldCount());
        return false;
    }
    for (uint32_t i = 0; i < ft.getRecordCount(); ++i) {
        fmt::FactionTemplateRow r{};
        r.id = ft.getUInt32(i, kFtId);
        r.faction = ft.getUInt32(i, kFtFaction);
        r.factionGroup = ft.getUInt32(i, kFtGroup);
        r.friendGroup = ft.getUInt32(i, kFtFriend);
        r.enemyGroup = ft.getUInt32(i, kFtEnemy);
        for (uint32_t e = 0; e < 4; ++e) r.enemy[e] = ft.getUInt32(i, kFtEnemy0 + e);
        tOut.push_back(r);
    }
    uint32_t skipped = 0;
    for (uint32_t i = 0; i < fac.getRecordCount(); ++i) {
        fmt::FactionRow r{};
        r.id = fac.getUInt32(i, kFacId);
        r.repListId = fac.getUInt32(i, kFacRepList);
        if (r.repListId == fmt::kNoRepList) {  // upstream: "no standing to have"
            ++skipped;
            continue;
        }
        for (uint32_t s = 0; s < 4; ++s) {
            r.raceMask[s] = fac.getUInt32(i, kFacRace0 + s);
            r.classMask[s] = fac.getUInt32(i, kFacClass0 + s);
            r.base[s] = fac.getInt32(i, kFacBase0 + s);
            r.flags[s] = fac.getUInt32(i, kFacFlags0 + s);
        }
        fOut.push_back(r);
    }
    // Upstream looks rows up by id (the first match in file order for the
    // player's template): sorted, a duplicate id would be ambiguous.
    auto byId = [](const auto& a, const auto& b) { return a.id < b.id; };
    std::stable_sort(tOut.begin(), tOut.end(), byId);
    std::stable_sort(fOut.begin(), fOut.end(), byId);
    auto dedupe = [](auto& v) {
        const size_t n = v.size();
        v.erase(std::unique(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.id == b.id; }), v.end());
        return n - v.size();
    };
    const size_t dupT = dedupe(tOut), dupF = dedupe(fOut);
    std::printf("factions: %zu templates (%u fields), %zu factions with a standing (%u without, %u fields)\n",
                tOut.size(), ft.getFieldCount(), fOut.size(), skipped, fac.getFieldCount());
    if (dupT || dupF) std::printf("warning: duplicate ids dropped: templates %zu, factions %zu\n", dupT, dupF);
    return true;
}

void dumpTsv(const std::string& path, const fmt::FactionTableView& v) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f, "# template\tid\tfaction\tgroup\tfriend\tenemy\tenemy0\tenemy1\tenemy2\tenemy3\n");
    for (uint32_t i = 0; i < v.h->templateCount; ++i) {
        const auto& t = v.templates[i];
        std::fprintf(f, "T\t%u\t%u\t0x%X\t0x%X\t0x%X\t%u\t%u\t%u\t%u\n", t.id, t.faction, t.factionGroup,
                     t.friendGroup, t.enemyGroup, t.enemy[0], t.enemy[1], t.enemy[2], t.enemy[3]);
    }
    std::fprintf(f, "# faction\tid\trepList\traces0..3\tclasses0..3\tbase0..3\tflags0..3\n");
    for (uint32_t i = 0; i < v.h->factionCount; ++i) {
        const auto& r = v.factions[i];
        std::fprintf(f, "F\t%u\t%u\t0x%X,0x%X,0x%X,0x%X\t0x%X,0x%X,0x%X,0x%X\t%d,%d,%d,%d\t0x%X,0x%X,0x%X,0x%X\n", r.id,
                     r.repListId, r.raceMask[0], r.raceMask[1], r.raceMask[2], r.raceMask[3], r.classMask[0],
                     r.classMask[1], r.classMask[2], r.classMask[3], r.base[0], r.base[1], r.base[2], r.base[3],
                     r.flags[0], r.flags[1], r.flags[2], r.flags[3]);
    }
    std::fclose(f);
    std::printf("dump: %s\n", path.c_str());
}

// Each playable race: its own template friendly, the other side's player
// templates hostile (every 3.3.5a client agrees).
int verify(const fmt::FactionTableView& v, bool quiet = false) {
    static const uint8_t kRaces[] = {1, 2, 3, 4, 5, 6, 7, 8, 10, 11};
    auto alliance = [](uint8_t r) { return r == 1 || r == 3 || r == 4 || r == 7 || r == 11; };
    int errors = 0;
    for (uint8_t race : kRaces) {
        fmt::FactionReactions m;
        fmt::buildFactionReactions(v, race, m);
        if (!quiet)
            std::printf("verify race %u (FT %u): hostile %u/%zu, friendly %u, standings %u (%u at war), "
                        "friendGroup=0x%X enemyGroup=0x%X\n",
                        race, m.playerFtId, m.hostileCount, m.templates.size(), m.friendlyCount, m.repFactions,
                        m.atWarCount, m.playerFriendGroup, m.playerEnemyGroup);
        const fmt::FactionReaction* own = m.find(m.playerFtId);
        if (!own || own->hostile) {
            std::printf("verify MISMATCH race %u: own template %u %s\n", race, m.playerFtId,
                        own ? "hostile" : "missing");
            ++errors;
        }
        for (uint8_t other : kRaces) {
            if (alliance(other) == alliance(race)) continue;
            const fmt::FactionReaction* r = m.find(fmt::playerFactionTemplate(other));
            if (!r || !r->hostile) {
                std::printf("verify MISMATCH race %u: race %u template %u %s\n", race, other,
                            fmt::playerFactionTemplate(other), r ? "not hostile" : "missing");
                ++errors;
            }
        }
    }
    std::printf("verify factions: %d errors\n", errors);
    return errors;
}

// Synthetic tables (group bits as the real client uses them: 1 player,
// 2 Alliance, 4 Horde, 8 monster).
int selftest() {
    std::vector<fmt::FactionTemplateRow> t;
    auto tpl = [&](uint32_t id, uint32_t faction, uint32_t group, uint32_t fr, uint32_t en, uint32_t enemy0 = 0) {
        fmt::FactionTemplateRow r{};
        r.id = id;
        r.faction = faction;
        r.factionGroup = group;
        r.friendGroup = fr;
        r.enemyGroup = en;
        r.enemy[0] = enemy0;
        t.push_back(r);
    };
    tpl(1, 1, 3, 2, 12);         // Human player: player|Alliance, friends Alliance, enemies Horde|monster
    tpl(2, 2, 5, 4, 10);         // Orc player
    tpl(11, 72, 2, 2, 12);       // capital city guard
    tpl(14, 14, 8, 0, 7);        // monster
    tpl(35, 35, 0, 0, 0);        // friendly to all (no groups)
    tpl(38, 38, 0, 0, 0, 1);     // explicit enemy of faction 1 (Human)
    tpl(190, 190, 0, 0, 0);      // wildlife: neither
    tpl(1000, 900, 8, 2, 0);     // monster group but friend of Alliance -> step 4 not hostile
    tpl(1001, 901, 0, 0, 0);     // standing faction, hostile start
    tpl(1002, 902, 8, 0, 0);     // standing faction, peace forced, friendly
    std::vector<fmt::FactionRow> f;
    auto fac = [&](uint32_t id, uint32_t rep, uint32_t races, int32_t base, uint32_t flags) {
        fmt::FactionRow r{};
        r.id = id;
        r.repListId = rep;
        r.raceMask[0] = races;
        r.base[0] = base;
        r.flags[0] = flags;
        f.push_back(r);
    };
    fac(1, 0, 0x44D, 3000, 0);
    fac(901, 5, 0x44D, -4000, 0);                            // Hostile start, at war
    fac(902, 6, 0x44D, -1200, fmt::kFactionFlagPeaceForced); // Unfriendly, peace forced
    const std::vector<uint8_t> blob = buildBlob(t, f);
    fmt::FactionTableView v;
    if (!v.attach(blob.data(), blob.size())) {
        std::printf("selftest FAIL: attach\n");
        return 1;
    }
    fmt::FactionReactions m;
    fmt::buildFactionReactions(v, 1, m);
    struct Want {
        uint32_t id;
        int hostile, friendly;
    };
    const Want want[] = {{1, 0, 1}, {2, 1, 0}, {11, 0, 1}, {14, 1, 0}, {35, 0, 0}, {38, 1, 0},
                         {190, 0, 0}, {1000, 0, 1}, {1001, 1, 0}, {1002, 0, 1}};
    int bad = 0;
    for (const Want& w : want) {
        const fmt::FactionReaction* r = m.find(w.id);
        if (!r || r->hostile != w.hostile || r->friendly != w.friendly) {
            std::printf("selftest FAIL: template %u hostile=%d friendly=%d want %d/%d\n", w.id, r ? r->hostile : -1,
                        r ? r->friendly : -1, w.hostile, w.friendly);
            ++bad;
        }
    }
    const fmt::FactionReaction* rep = m.find(1001);
    if (!rep || rep->repListId != 5) {
        std::printf("selftest FAIL: template 1001 repList\n");
        ++bad;
    }
    if (fmt::factionBaseReputation(*v.faction(901), 1, 1) != -4000 ||
        fmt::factionBaseReputation(*v.faction(901), 2, 1) != 0) {
        std::printf("selftest FAIL: base reputation\n");
        ++bad;
    }
    // Truncated / unsorted blobs are refused.
    fmt::FactionTableView bad1;
    if (bad1.attach(blob.data(), blob.size() - 1)) {
        std::printf("selftest FAIL: truncated blob accepted\n");
        ++bad;
    }
    std::vector<uint8_t> swapped = blob;
    std::swap_ranges(swapped.begin() + 16, swapped.begin() + 16 + 36, swapped.begin() + 16 + 36);
    if (bad1.attach(swapped.data(), swapped.size())) {
        std::printf("selftest FAIL: unsorted blob accepted\n");
        ++bad;
    }
    std::printf("selftest: %s (human: hostile %u, friendly %u of %zu)\n", bad ? "FAILED" : "OK", m.hostileCount,
                m.friendlyCount, m.templates.size());
    return bad ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string root, out, dump;
    bool doVerify = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--extracted" && i + 1 < argc) root = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--dump" && i + 1 < argc) dump = argv[++i];
        else if (a == "--verify") doVerify = true;
        else if (a == "--selftest") return selftest();
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }
    if (root.empty() || out.empty()) {
        std::fprintf(stderr, "usage: psp_dbc_conv --extracted <dir> --out <dbc.pak> [--verify] [--dump <tsv>]\n"
                             "       psp_dbc_conv --selftest\n");
        return 2;
    }
    std::vector<fmt::FactionTemplateRow> templates;
    std::vector<fmt::FactionRow> factions;
    if (!readTables(root, templates, factions)) return 1;
    std::vector<PakItem> items;
    items.push_back({"factions", 3, buildBlob(templates, factions)});
    std::printf("factions: %zu bytes\n", items.back().blob.size());
    if (!writePak(out, items)) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
    }
    if (!doVerify && dump.empty()) return 0;
    // Back through the PSP's reader.
    std::vector<uint8_t> file;
    if (!readFile(out, file) || file.size() < 16 + 64) {
        std::fprintf(stderr, "cannot read back %s\n", out.c_str());
        return 3;
    }
    uint32_t count = 0, tocOff = 0;
    std::memcpy(&count, file.data() + 8, 4);
    std::memcpy(&tocOff, file.data() + 12, 4);
    const uint8_t* blob = nullptr;
    uint32_t size = 0;
    for (uint32_t i = 0; i < count && tocOff + (i + 1) * 64u <= file.size(); ++i) {
        const uint8_t* e = file.data() + tocOff + i * 64u;
        if (std::strncmp(reinterpret_cast<const char*>(e), "factions", 48) != 0) continue;
        uint32_t off = 0;
        std::memcpy(&off, e + 48, 4);
        std::memcpy(&size, e + 52, 4);
        if (uint64_t{off} + size <= file.size()) blob = file.data() + off;
    }
    fmt::FactionTableView v;
    if (!blob || !v.attach(blob, size)) {
        std::fprintf(stderr, "verify: \"factions\" missing or refused by FactionTableView\n");
        return 3;
    }
    if (!dump.empty()) dumpTsv(dump, v);
    if (doVerify && verify(v) != 0) return 3;
    return 0;
}
