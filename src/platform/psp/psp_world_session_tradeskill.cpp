// the trade skill window's half in the session, as upstream does it:
//   - SpellHandler::tradeskillOpenerSkillLine / openCraftingWindow / closeCraftingWindow: casting a
//     profession spell (SpellEffect 47 TRADE_SKILL, or named after its SkillLine, in a category 9 / 11
//     line, with at least one known recipe) opens the window client-side instead of being sent;
//   - SpellHandler::startCraftQueue / cancelCraftQueue and handleSpellGo's re-cast: "Create" with a count
//     casts the recipe again on each SMSG_SPELL_GO until the count is made; a failed / cancelled cast drops
//     the queue (clearOwnCast);
//   - GameHandler::getCraftingRecipes / countItemInBags: the known recipes of the open line, by name, with
//     how many the bags have reagents for.
// PSP: the recipe data is data/craft.pak (CraftDb, tools/psp/build_craft_pak.py) instead of the Spell.dbc /
// SkillLineAbility.dbc caches, loaded for the open line only and freed when the window closes; the window's
// data asks for its items (names) on demand. The difficulty bands are the client's and the server's
// (getRecipeDifficulty below), not upstream's.
#include "platform/psp/psp_world_session.hpp"

#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_spell_db.hpp"
#include "platform/psp/psp_strings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace wowee::platform::psp {

namespace {
// SkillLine.dbc categories that hold crafting recipes (upstream tradeskillOpenerSkillLine).
constexpr uint16_t kCatSecondary = 9;    // Cooking, First Aid, Fishing
constexpr uint16_t kCatProfession = 11;  // Alchemy, Blacksmithing, ...
// What "this spell opens the trade skill window" is written as in Spell.dbc.
constexpr uint8_t kSpellEffectTradeSkill = 47;
// PSP: upstream warns on every cast of a profession spell that is not an opener - every craft of a recipe; the
// card log gets the first few.
uint32_t g_notOpenerLogs = 0;
}  // namespace

// --- CraftDb ------------------------------------------------------------------------------

bool CraftDb::load(uint32_t skillLine) {
    release();
    char path[256];
    if (!fsPath(path, sizeof(path), "data/craft.pak")) return false;
    Pak pak;
    if (!pak.open(path)) {
        noPak_ = true;
        return false;
    }
    noPak_ = false;
    char name[24];
    std::snprintf(name, sizeof(name), "cl_%u", static_cast<unsigned>(skillLine));
    const PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    const uint32_t size = e ? e->size : 0;
    pak.close();
    if (!blob) return false;
    const auto* h = static_cast<const CraftLineHeader*>(blob);
    const uint32_t body = sizeof(CraftLineHeader) + h->recipeCount * sizeof(CraftRecipeRec) +
                          h->reagentCount * sizeof(CraftReagentRec) + h->itemCount * sizeof(CraftItemRec) +
                          h->groupCount * sizeof(CraftGroupRec) + h->slotCount * sizeof(CraftSlotRec);
    if (size < sizeof(CraftLineHeader) || std::memcmp(h->magic, "WCRL", 4) != 0 || h->version != 1 ||
        body > size || h->strOff < body || h->strOff + h->strSize > size || h->strSize == 0 ||
        static_cast<const char*>(blob)[h->strOff + h->strSize - 1] != '\0') {
        PSP_LOGW(tag::Net, "craft_line_bad line=%u size=%u", static_cast<unsigned>(skillLine),
                 static_cast<unsigned>(size));
        pakFree(blob);
        return false;
    }
    blob_ = blob;
    size_ = size;
    hdr_ = h;
    const auto* p = static_cast<const uint8_t*>(blob) + sizeof(CraftLineHeader);
    recipes_ = reinterpret_cast<const CraftRecipeRec*>(p);
    p += h->recipeCount * sizeof(CraftRecipeRec);
    reagents_ = reinterpret_cast<const CraftReagentRec*>(p);
    p += h->reagentCount * sizeof(CraftReagentRec);
    items_ = reinterpret_cast<const CraftItemRec*>(p);
    p += h->itemCount * sizeof(CraftItemRec);
    groups_ = reinterpret_cast<const CraftGroupRec*>(p);
    p += h->groupCount * sizeof(CraftGroupRec);
    slots_ = reinterpret_cast<const CraftSlotRec*>(p);
    strings_ = static_cast<const char*>(blob) + h->strOff;
    strSize_ = h->strSize;
    return true;
}

void CraftDb::release() {
    if (blob_) pakFree(blob_);
    blob_ = nullptr;
    size_ = 0;
    hdr_ = nullptr;
    recipes_ = nullptr;
    reagents_ = nullptr;
    items_ = nullptr;
    groups_ = nullptr;
    slots_ = nullptr;
    strings_ = nullptr;
    strSize_ = 0;
}

void CraftDb::swap(CraftDb& o) {
    std::swap(blob_, o.blob_);
    std::swap(size_, o.size_);
    std::swap(hdr_, o.hdr_);
    std::swap(recipes_, o.recipes_);
    std::swap(reagents_, o.reagents_);
    std::swap(items_, o.items_);
    std::swap(groups_, o.groups_);
    std::swap(slots_, o.slots_);
    std::swap(strings_, o.strings_);
    std::swap(strSize_, o.strSize_);
    std::swap(noPak_, o.noPak_);
}

const CraftRecipeRec* CraftDb::recipe(uint32_t spellId) const {
    if (!hdr_) return nullptr;
    const CraftRecipeRec* end = recipes_ + hdr_->recipeCount;
    const CraftRecipeRec* it =
        std::lower_bound(recipes_, end, spellId, [](const CraftRecipeRec& r, uint32_t id) { return r.spellId < id; });
    return it != end && it->spellId == spellId ? it : nullptr;
}

uint32_t CraftDb::displayOf(uint32_t itemId) const {
    if (!hdr_) return 0;
    const CraftItemRec* end = items_ + hdr_->itemCount;
    const CraftItemRec* it =
        std::lower_bound(items_, end, itemId, [](const CraftItemRec& r, uint32_t id) { return r.item < id; });
    return it != end && it->item == itemId ? it->displayId : 0;
}

// --- The window -----------------------------------------------------------------------------

// upstream SpellHandler::tradeskillOpenerSkillLine. PSP: the "at least one known recipe" test reads the
// line's craft.pak entry (kept when the window then opens on it).
uint32_t WorldSession::tradeSkillOpenerLine(uint32_t spellId) {
    if (!spellDb_) return 0;
    const SpellInfo* info = spellInfo(spellId);
    if (!info || info->rec.skillLine == 0) return 0;
    const uint32_t skillLine = info->rec.skillLine;
    const SpellDb::SkillLineInfo* line = spellDb_->skillLine(skillLine);
    if (!line) {
        PSP_LOGW(tag::Net, "craft_opener spell=%u line=%u no_category", static_cast<unsigned>(spellId),
                 static_cast<unsigned>(skillLine));
        return 0;
    }
    if (line->category != kCatSecondary && line->category != kCatProfession) return 0;
    // Spell.dbc says which spells open this window: effect 47. The name test is the second answer (an edited
    // Spell.dbc of a private server), as upstream.
    bool opensWindow = false;
    for (uint8_t effect : info->rec.effect)
        if (effect == kSpellEffectTradeSkill) opensWindow = true;
    if (!opensWindow) opensWindow = !info->name.empty() && info->name == line->name;
    if (!opensWindow) {
        if (++g_notOpenerLogs <= 5)
            PSP_LOGW(tag::Net, "craft_opener spell=%u line=%u not_an_opener", static_cast<unsigned>(spellId),
                     static_cast<unsigned>(skillLine));
        return 0;
    }
    // Only open a window that will actually list something (this keeps Fishing a normal cast).
    CraftDb probe;
    const CraftDb* db = &craftDb_;
    if (!craftDb_.loaded() || craftDb_.skillLine() != skillLine) {
        if (!probe.load(skillLine)) {
            if (probe.noPak()) spellError(OWN_STR(CRAFT_PAK_MISSING));
            PSP_LOGW(tag::Net, "craft_opener spell=%u line=%u no_recipes pak=%d", static_cast<unsigned>(spellId),
                     static_cast<unsigned>(skillLine), probe.noPak() ? 0 : 1);
            return 0;
        }
        db = &probe;
    }
    bool any = false;
    for (uint32_t i = 0; i < db->recipeCount() && !any; ++i) {
        const uint32_t id = db->recipeAt(i).spellId;
        any = id != spellId && knownSpells_.count(id) != 0;
    }
    if (!any) {
        PSP_LOGW(tag::Net, "craft_opener spell=%u line=%u no_known_recipe", static_cast<unsigned>(spellId),
                 static_cast<unsigned>(skillLine));
        return 0;
    }
    if (db == &probe) craftDb_.swap(probe);  // the old line (if any) goes with the probe
    return skillLine;
}

// upstream SpellHandler::openCraftingWindow (TRADE_SKILL_SHOW).
void WorldSession::openCraftingWindow(uint32_t skillLine) {
    if (!craftDb_.loaded() || craftDb_.skillLine() != skillLine) {
        CraftDb probe;
        if (!probe.load(skillLine)) return;
        craftDb_.swap(probe);
    }
    craftingWindowOpen_ = true;
    craftingSkillLine_ = skillLine;
    ++craftShows_;
    uint32_t known = 0;
    for (uint32_t i = 0; i < craftDb_.recipeCount(); ++i) known += knownSpells_.count(craftDb_.recipeAt(i).spellId);
    PSP_LOGI(tag::Net, "craft_open line=%u recipes=%u known=%u kb=%u", static_cast<unsigned>(skillLine),
             static_cast<unsigned>(craftDb_.recipeCount()), static_cast<unsigned>(known),
             static_cast<unsigned>((craftDb_.bytes() + 1023u) / 1024u));
}

// upstream SpellHandler::closeCraftingWindow (TRADE_SKILL_CLOSE). PSP: the line's data goes with it.
void WorldSession::closeCraftingWindow() {
    if (!craftingWindowOpen_) return;
    craftingWindowOpen_ = false;
    PSP_LOGI(tag::Net, "craft_close line=%u kb=%u", static_cast<unsigned>(craftingSkillLine_),
             static_cast<unsigned>((craftDb_.bytes() + 1023u) / 1024u));
    craftDb_.release();
}

// GameHandler::getRecipeDifficulty. PSP: the bands of the client and the server -
// AzerothCore Player::UpdateCraftSkill's SkillGainChance(value, grey = TrivialSkillLineRankHigh,
// green = (High + Low) / 2, yellow = TrivialSkillLineRankLow), which is what the recipe's orange / yellow /
// green / grey are (Linen Bandage: SkillLineAbility 60 / 30 -> yellow at 30, green at 45, grey at 60).
// Upstream greens at Low and yellows half way from MinSkillLineRank to Low.
int WorldSession::getRecipeDifficulty(const CraftRecipeRec& r, uint32_t skill) {
    if (r.trivialHigh == 0 && r.trivialLow == 0) return 0;  // no thresholds: never greys out
    if (skill >= r.trivialHigh) return 3;
    if (skill >= (static_cast<uint32_t>(r.trivialHigh) + r.trivialLow) / 2u) return 2;
    if (skill >= r.trivialLow) return 1;
    return 0;
}

uint32_t WorldSession::countItemInBags(uint32_t itemId) const { return getInventory().countItem(itemId); }

// GetTradeSkillLine's rank / maxRank: the line's skill (effectiveValue = value + both bonuses, as upstream).
bool WorldSession::craftSkill(uint32_t& rank, uint32_t& maxRank) {
    rank = maxRank = 0;
    for (const PlayerSkill& s : playerSkills()) {
        if (s.skillId != craftingSkillLine_) continue;
        rank = static_cast<uint32_t>(s.value) + s.bonusTemp + s.bonusPerm;
        maxRank = s.maxValue;
        return true;
    }
    return false;
}

// upstream GameHandler::getCraftingRecipes.
std::vector<WorldSession::CraftRecipe> WorldSession::getCraftingRecipes() {
    std::vector<CraftRecipe> out;
    if (!craftingWindowOpen_ || !craftDb_.loaded() || !spellDb_) return out;
    uint32_t skill = 0, maxRank = 0;
    craftSkill(skill, maxRank);
    // PSP: each reagent counted once per call (Inventory::countItem walks every bag slot).
    std::unordered_map<uint32_t, uint32_t> inBags;
    auto have = [&](uint32_t item) {
        auto it = inBags.find(item);
        if (it == inBags.end()) it = inBags.emplace(item, countItemInBags(item)).first;
        return it->second;
    };
    for (uint32_t i = 0; i < craftDb_.recipeCount(); ++i) {
        const CraftRecipeRec& r = craftDb_.recipeAt(i);
        if (!knownSpells_.count(r.spellId)) continue;
        const SpellInfo* info = spellInfo(r.spellId);
        if (!info) continue;
        CraftRecipe row;
        row.spellId = r.spellId;
        row.name = info->name;
        row.difficulty = getRecipeDifficulty(r, skill);
        row.canMake = r.reagentCount ? 999 : 0;
        const CraftReagentRec* rg = craftDb_.reagents(r);
        for (uint8_t k = 0; k < r.reagentCount; ++k) {
            if (rg[k].item == 0 || rg[k].count == 0) continue;
            row.canMake = std::min(row.canMake, static_cast<int>(have(rg[k].item) / rg[k].count));
        }
        row.rec = &r;
        out.push_back(std::move(row));
    }
    std::sort(out.begin(), out.end(), [](const CraftRecipe& a, const CraftRecipe& b) { return a.name < b.name; });
    return out;
}

// --- The craft queue --------------------------------------------------------------------------

// upstream SpellHandler::startCraftQueue (PSP: no flying, so no airborne refusal).
void WorldSession::startCraftQueue(uint32_t spellId, int count, bool moving) {
    if (spellId == 0) return;
    craftQueueSpellId_ = spellId;
    craftQueueRemaining_ = count < 1 ? 1 : count;
    ++craftQueueVersion_;
    PSP_LOGI(tag::Net, "craft_queue spell=%u count=%d", static_cast<unsigned>(spellId), craftQueueRemaining_);
    castSpell(spellId, 0, moving);
}

void WorldSession::cancelCraftQueue() {
    if (craftQueueSpellId_ == 0 && craftQueueRemaining_ == 0) return;
    craftQueueSpellId_ = 0;
    craftQueueRemaining_ = 0;
    ++craftQueueVersion_;
}

// upstream handleSpellGo: "Craft queue: re-cast if more crafts remaining" (before the spell queue).
bool WorldSession::craftQueueSpellGo(uint32_t spellId) {
    if (craftQueueRemaining_ <= 0 || craftQueueSpellId_ != spellId) return false;
    --craftQueueRemaining_;
    ++craftQueueVersion_;  // UPDATE_TRADESKILL_RECAST: the count box counts down
    if (craftQueueRemaining_ > 0) {
        PSP_LOGI(tag::Net, "craft_recast spell=%u left=%d", static_cast<unsigned>(spellId), craftQueueRemaining_);
        castSpell(craftQueueSpellId_, 0, false);
    } else {
        craftQueueSpellId_ = 0;
    }
    return true;
}

}  // namespace wowee::platform::psp
