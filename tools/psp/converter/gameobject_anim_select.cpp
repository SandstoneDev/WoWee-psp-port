// Clean-room implementation (cleanroom/clean/equip) of the spec gameobject_anim_select.md section 3 and the `goanim`
// record of gameobject_rules.md section 2.4.
#include "gameobject_anim_select.hpp"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>

namespace wowee::pspconv::goanim {

namespace {

std::string lower(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

bool endsWithLower(const std::string& lowered, const char* suffix) {
    const std::string suf(suffix);
    return lowered.size() >= suf.size() && lowered.compare(lowered.size() - suf.size(), suf.size(), suf) == 0;
}

struct ModelInfo {
    bool readable = false;
    std::vector<uint16_t> stateAnims;
};

}  // namespace

bool goAnimPassRuns(const GoAnimOptions& options) {
    if (options.noGameObjects || options.humanoidCheck) return false;
    return options.allCreatureDisplays || options.gameObjects;
}

bool parseGoDisplayList(const std::string& arg, std::vector<uint32_t>& ids) {
    ids.clear();
    size_t pos = 0;
    while (pos <= arg.size()) {
        const size_t comma = arg.find(',', pos);
        const std::string tok = arg.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (tok.empty()) return false;
        for (char c : tok) {
            if (c < '0' || c > '9') return false;
        }
        const unsigned long v = std::strtoul(tok.c_str(), nullptr, 10);
        if (v > 0xFFFFFFFFul) return false;
        ids.push_back(static_cast<uint32_t>(v));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return !ids.empty();
}

std::string goModelPath(const std::string& modelName) {
    if (modelName.size() < 4) return std::string();
    const std::string l = lower(modelName);
    if (endsWithLower(l, ".wmo")) return std::string();
    if (endsWithLower(l, ".mdx") || endsWithLower(l, ".mdl")) {
        return modelName.substr(0, modelName.size() - 4) + ".m2";
    }
    return modelName;
}

std::vector<uint32_t> GoAnimSelection::selectedIds() const {
    std::vector<uint32_t> ids;
    ids.reserve(selected.size());
    for (const GoAnimSelected& s : selected) ids.push_back(s.displayId);
    return ids;
}

GoAnimSelection selectGameObjectAnims(const std::vector<GoDisplayRow>& rows, const GoAnimOptions& options,
                                      GoModelSource& models) {
    GoAnimSelection sel;
    std::unordered_map<std::string, ModelInfo> cache;  // by lower-cased path: each path is read at most once

    for (const GoDisplayRow& row : rows) {
        if (options.hasDisplayFilter &&
            std::find(options.displayFilter.begin(), options.displayFilter.end(), row.id) ==
                options.displayFilter.end()) {
            continue;
        }
        const std::string path = goModelPath(row.modelName);
        if (path.empty()) continue;
        const std::string key = lower(path);

        auto it = cache.find(key);
        if (it == cache.end()) {
            ModelInfo info;
            std::vector<uint16_t> seq;
            info.readable = models.readSequenceIds(path, seq) == ModelReadResult::kOk;
            ++sel.modelsRead;
            if (!info.readable) {
                ++sel.modelsUnreadable;
            } else {
                for (uint16_t id : seq) {
                    if (id >= kFirstStateAnim && id <= kLastStateAnim) info.stateAnims.push_back(id);
                }
                std::sort(info.stateAnims.begin(), info.stateAnims.end());
                info.stateAnims.erase(std::unique(info.stateAnims.begin(), info.stateAnims.end()),
                                      info.stateAnims.end());
            }
            it = cache.emplace(key, std::move(info)).first;
        }
        if (it->second.stateAnims.empty()) continue;  // S empty (or unreadable model): not selected

        GoAnimSelected s;
        s.displayId = row.id;
        s.modelPath = path;
        s.stateAnims = it->second.stateAnims;
        sel.selected.push_back(std::move(s));
        sel.extraSequencesByModel[key] = it->second.stateAnims;
    }
    std::stable_sort(sel.selected.begin(), sel.selected.end(),
                     [](const GoAnimSelected& a, const GoAnimSelected& b) { return a.displayId < b.displayId; });
    return sel;
}

std::string goAnimDisplayEntryName(uint32_t gameObjectDisplayId) {
    return "d_" + std::to_string(goAnimDisplayId(gameObjectDisplayId));
}

std::vector<uint8_t> writeGoAnimRecord(std::vector<uint32_t> bakedIds) {
    std::sort(bakedIds.begin(), bakedIds.end());
    std::vector<uint8_t> b;
    b.reserve(8 + 4 * bakedIds.size());
    const char magic[4] = {'W', 'G', 'O', 'A'};
    for (char c : magic) b.push_back(static_cast<uint8_t>(c));
    auto u32 = [&b](uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    };
    u32(static_cast<uint32_t>(bakedIds.size()));
    for (uint32_t id : bakedIds) u32(id);
    return b;
}

}  // namespace wowee::pspconv::goanim
