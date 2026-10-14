// the interface's strings - see include/platform/psp/psp_strings.hpp.
#include "platform/psp/psp_strings.hpp"

#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_locale.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_strings_table.hpp"

#include <pspthreadman.h>

#include <cstdio>
#include <string>
#include <vector>

namespace wowee::platform::psp {

namespace {

constexpr size_t kCsCount = static_cast<size_t>(CS::kCount);
constexpr size_t kPsCount = static_cast<size_t>(PS::kCount);

const char* const kCsKeys[kCsCount + 1] = {
#define WOWEE_CS(key) #key,
#include "platform/psp/strings/cs_list.inc"
#undef WOWEE_CS
    ""};

const char* const kPsEn[kPsCount + 1] = {
#define WOWEE_PS(id, en, ru) en,
#include "platform/psp/strings/ps_list.inc"
#undef WOWEE_PS
    ""};

const char* const kPsRu[kPsCount + 1] = {
#define WOWEE_PS(id, en, ru) ru,
#include "platform/psp/strings/ps_list.inc"
#undef WOWEE_PS
    ""};

std::string g_pool;                    // the texts read (resident for the run)
std::vector<int32_t> g_at;             // CS -> offset in g_pool, -1 = no pak had it
const char* g_cs[kCsCount + 1] = {};   // CS -> text (the key while missing)
bool g_ownRu = true;

bool pakRead(void* ctx, uint32_t offset, void* dst, uint32_t size) {
    auto* p = static_cast<std::pair<Pak*, const PakEntry*>*>(ctx);
    return p->first->readPart(p->second, offset, dst, size);
}

// The wanted keys still missing, read from one strings.pak. Returns the texts found.
uint32_t loadFrom(const char* absPath, const char* which, char localeOut[5]) {
    std::vector<text::WstrWant> wants;
    for (size_t i = 0; i < kCsCount; ++i)
        if (g_at[i] < 0) wants.push_back(text::WstrWant{text::fnv1a64(kCsKeys[i]), static_cast<uint32_t>(i)});
    if (wants.empty()) return 0;
    Pak pak;
    if (!pak.open(absPath)) return 0;
    const PakEntry* e = pak.find("strings");
    if (!e) {
        PSP_LOGW(tag::Fs, "strings_pak_bad %s path=%s (no \"strings\" entry)", which, absPath);
        pak.close();
        return 0;
    }
    std::pair<Pak*, const PakEntry*> ctx{&pak, e};
    const text::WstrResult r = text::wstrLoad(&pakRead, &ctx, e->size, wants, g_pool, g_at);
    pak.close();
    if (!r.ok) {
        PSP_LOGW(tag::Fs, "strings_pak_bad %s path=%s (header)", which, absPath);
        return 0;
    }
    std::snprintf(localeOut, 5, "%s", r.locale);
    PSP_LOGI(tag::Fs, "strings_pak %s locale=%s keys=%u wanted=%u found=%u reads=%u", which, r.locale,
             static_cast<unsigned>(r.count), static_cast<unsigned>(wants.size()), static_cast<unsigned>(r.found),
             static_cast<unsigned>(r.reads));
    return r.found;
}

}  // namespace

bool stringsInit() {
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    g_ownRu = localeActive() == "ruRU";
    g_pool.clear();
    g_at.assign(kCsCount, -1);
    char path[256];
    char loc[5] = "----";
    // The running language's pak (fsPath: data/locale/<xxYY>/strings.pak when it has one), then the primary's.
    uint32_t found = fsPath(path, sizeof(path), "data/strings.pak") ? loadFrom(path, "active", loc) : 0;
    char primLoc[5] = "----";
    uint32_t fromPrimary = 0;
    if (found < kCsCount && localeActive() != localePrimary() &&
        localePakPath(localePrimary(), "strings.pak", path, sizeof(path)))
        fromPrimary = loadFrom(path, "primary", primLoc);
    // Pointers only now: the pool does not move any more.
    std::string missing;
    uint32_t nMissing = 0;
    for (size_t i = 0; i < kCsCount; ++i) {
        if (g_at[i] >= 0) {
            g_cs[i] = g_pool.data() + g_at[i];
        } else {
            g_cs[i] = kCsKeys[i];  // the key itself: visible, never a crash
            ++nMissing;
            if (missing.size() < 70) missing += (missing.empty() ? "" : ",") + std::string(kCsKeys[i]);
        }
    }
    g_cs[kCsCount] = "";
    PSP_LOGI(tag::Fs, "strings_load locale=%s keys=%u found=%u primary=%u missing=%u kb=%u own=%s ms=%u", loc,
             static_cast<unsigned>(kCsCount), static_cast<unsigned>(found), static_cast<unsigned>(fromPrimary),
             static_cast<unsigned>(nMissing), static_cast<unsigned>((stringsResidentBytes() + 1023) / 1024),
             g_ownRu ? "ru" : "en", static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    if (nMissing)
        PSP_LOGW(tag::Fs, "strings_missing n=%u keys=%s%s", static_cast<unsigned>(nMissing), missing.c_str(),
                 nMissing > 6 ? ",..." : "");
    return nMissing == 0;
}

const char* cs(CS id) {
    const size_t i = static_cast<size_t>(id);
    if (i >= kCsCount) return "";
    return g_cs[i] ? g_cs[i] : kCsKeys[i];  // before stringsInit: the key
}

const char* csKey(CS id) {
    const size_t i = static_cast<size_t>(id);
    return i < kCsCount ? kCsKeys[i] : "";
}

bool csMissing(CS id) {
    const size_t i = static_cast<size_t>(id);
    return i >= kCsCount || i >= g_at.size() || g_at[i] < 0;
}

const char* ps(PS id) {
    const size_t i = static_cast<size_t>(id);
    if (i >= kPsCount) return "";
    return g_ownRu ? kPsRu[i] : kPsEn[i];
}

uint32_t stringsResidentBytes() {
    return static_cast<uint32_t>(g_pool.capacity() + g_at.capacity() * sizeof(int32_t) + sizeof(g_cs));
}

}  // namespace wowee::platform::psp
