#pragma once
// PC seam for upstream's pipeline::AssetManager.
//
// psp_equip_preview --matrix compiles upstream's own equipment code
// (src/core/entity_spawner*.cpp, appearance_composer.cpp, helm_visual.cpp,
// pipeline/item_textures.cpp, m2_asset_loader.cpp) unchanged, with this
// directory first on the include path. This header stands in for the real
// AssetManager (a manifest.json asset tree): the same calls, answered from
// psp_mpq_extract folders (lower-case paths) - the extracted tree and fast
// copies of parts of it (--mirror). Nothing else about upstream changes.
#include "pipeline/blp_loader.hpp"
#include "pipeline/dbc_loader.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace pipeline {

class AssetManager {
public:
    AssetManager() = default;

    // matrix: `roots` are searched in order; a folder is listed once from the
    // first root that has it (a mirror must hold the whole folder it copies).
    bool initialize(const std::vector<std::string>& roots);
    [[nodiscard]] bool isInitialized() const { return initialized_; }

    BLPImage loadTexture(const std::string& path, bool keepCompressed = false);
    std::shared_ptr<DBCFile> loadDBC(const std::string& name);
    std::shared_ptr<DBCFile> loadDBCOptional(const std::string& name) { return loadDBC(name); }
    [[nodiscard]] bool fileExists(const std::string& path) const;
    [[nodiscard]] std::vector<uint8_t> readFile(const std::string& path) const;
    [[nodiscard]] std::vector<uint8_t> readFileOptional(const std::string& path) const { return readFile(path); }
    [[nodiscard]] std::string resolveFile(const std::string& normalizedPath) const;
    void clearCache() { files_.clear(); }
    void clearDBCCache() { dbcs_.clear(); }

    // matrix: "dir\\File.M2" -> "dir/file.m2"
    static std::string normalize(const std::string& path);
    // matrix: reads are kept (an item model is asked for once per race and sex).
    void setReadCacheLimit(size_t bytes) { cacheLimit_ = bytes; }
    // matrix: the last .m2 read (loadM2WithSkin names a model by its internal name, which does not say
    // which racial cut or folder was loaded; attachWeapon takes the model read just before it).
    const std::string& lastModelRead() const { return lastM2_; }

private:
    const std::unordered_set<std::string>* listing(const std::string& norm, std::string& rootOut) const;

    bool initialized_ = false;
    std::vector<std::string> roots_;
    // folder (lower case, '/'-terminated) -> (root, files in it)
    mutable std::map<std::string, std::pair<std::string, std::unordered_set<std::string>>> folders_;
    std::unordered_map<std::string, std::shared_ptr<DBCFile>> dbcs_;
    mutable std::unordered_map<std::string, std::vector<uint8_t>> files_;
    mutable size_t cacheBytes_ = 0;
    mutable std::string lastM2_;
    size_t cacheLimit_ = size_t{512} * 1024 * 1024;
};

}  // namespace pipeline
}  // namespace wowee
