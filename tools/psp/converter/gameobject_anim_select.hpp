// Selection of game-object displays baked with their object-state animations (converter, PC).
//
// Clean-room implementation (cleanroom/clean/equip) from the specs gameobject_anim_select.md sections 2 and 3, with
// the `goanim` record and kGoAnimDisplayBase of gameobject_rules.md sections 2.4 and 3.10.
// Model files are read through our own interface (GoModelSource) so that tests can feed the vector inputs; baking
// the display records themselves (model, extra sequences, scale) stays with the existing converter.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wowee::pspconv::goanim {

constexpr uint32_t kGoAnimDisplayBase = 0xF00000;  // gameobject_rules.md 3.10
constexpr uint16_t kFirstStateAnim = 145;          // AnimationData.dbc: Spawn
constexpr uint16_t kLastStateAnim = 157;           // AnimationData.dbc: Despawn

struct GoDisplayRow {        // GameObjectDisplayInfo.dbc
    uint32_t id = 0;         // 0
    std::string modelName;   // 1
};

enum class ModelReadResult { kOk, kMissing, kNotM2 };

class GoModelSource {
public:
    virtual ~GoModelSource() = default;
    // Reads the model at `path` (case-insensitive data lookup). kOk only when the file exists and starts with
    // "MD20"; then `sequenceIds` holds the ids of its sequences (all variations).
    virtual ModelReadResult readSequenceIds(const std::string& path, std::vector<uint16_t>& sequenceIds) = 0;
};

struct GoAnimOptions {
    bool noGameObjects = false;         // --no-gameobjects
    bool gameObjects = false;           // --gameobjects
    bool allCreatureDisplays = false;   // every creature display is baked (no display filter)
    bool humanoidCheck = false;         // the humanoid-check mode
    bool hasDisplayFilter = false;      // --go-display given
    std::vector<uint32_t> displayFilter;
};

// Section 3: the pass runs when game objects are not disabled and either all creature displays are baked or
// --gameobjects is given, and not in the humanoid-check mode.
bool goAnimPassRuns(const GoAnimOptions& options);

// "N[,N...]" of --go-display; false on a malformed list.
bool parseGoDisplayList(const std::string& arg, std::vector<uint32_t>& ids);

// Model path of a row; "" = none.
std::string goModelPath(const std::string& modelName);

struct GoAnimSelected {
    uint32_t displayId = 0;
    std::string modelPath;
    std::vector<uint16_t> stateAnims;  // S: ascending, distinct, all in 145..157
};

struct GoAnimSelection {
    std::vector<GoAnimSelected> selected;  // in ascending display id order
    size_t modelsRead = 0;                 // distinct lower-cased model paths read
    size_t modelsUnreadable = 0;           // of those, missing or not "MD20"
    // Per distinct lower-cased model path of a selected row: the sequences its baked animation set gains (S).
    std::map<std::string, std::vector<uint16_t>> extraSequencesByModel;

    std::vector<uint32_t> selectedIds() const;
};

GoAnimSelection selectGameObjectAnims(const std::vector<GoDisplayRow>& rows, const GoAnimOptions& options,
                                      GoModelSource& models);

// The creature display id / entry name of a selected display.
inline uint32_t goAnimDisplayId(uint32_t gameObjectDisplayId) { return kGoAnimDisplayBase + gameObjectDisplayId; }
std::string goAnimDisplayEntryName(uint32_t gameObjectDisplayId);  // "d_<kGoAnimDisplayBase + id>"

// The "goanim" record: "WGOA", u32 count, u32 ids[count] ascending (the selected ids whose model baked).
std::vector<uint8_t> writeGoAnimRecord(std::vector<uint32_t> bakedIds);

}  // namespace wowee::pspconv::goanim
