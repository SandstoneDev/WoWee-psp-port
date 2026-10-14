// The character-select backgrounds (include/ui/psp/ui_glue_scene.hpp).
#include "ui/psp/ui_glue_scene.hpp"

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"

#include <pspthreadman.h>

#include <cstdio>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
using game::Class;
using game::Race;

const char* glueSceneFor(Race race, Class cls) {
    if (cls == Class::DEATH_KNIGHT) return "deathknight";
    switch (race) {
        case Race::HUMAN: return "human";
        case Race::ORC:
        case Race::TROLL: return "orc";
        case Race::DWARF:
        case Race::GNOME: return "dwarf";
        case Race::NIGHT_ELF: return "nightelf";
        case Race::UNDEAD: return "scourge";
        case Race::TAUREN: return "tauren";
        case Race::BLOOD_ELF: return "bloodelf";
        case Race::DRAENEI: return "draenei";
        default: break;
    }
    // PSP: UI_CharacterSelect, the scene of the 3.3.5a select screen before a character is chosen (the
    // client opens the creation screen when there are none; the PSP list stays with its New Character entry).
    return "characterselect";
}

void GlueBackground::release() {
    if (tex_.pixels) pp::textureRelease(tex_);
    have_ = false;
    name_[0] = '\0';
}

void GlueBackground::show(const char* scene) {
    if (!scene || std::strncmp(scene, name_, sizeof(name_)) == 0) return;
    release();
    std::snprintf(name_, sizeof(name_), "%s", scene);  // tried, even when the pak has no such entry
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    char path[256];
    pp::fsPath(path, sizeof(path), "data/glue.pak");
    pp::Pak pak;
    if (!pak.open(path)) return;
    bool found = false;
    if (const pp::PakEntry* e = pak.find("cs_scenes")) {
        if (void* blob = pak.load(e)) {
            const auto* b = static_cast<const uint8_t*>(blob);
            uint32_t n = 0;
            if (e->size >= 4) std::memcpy(&n, b, 4);
            for (uint32_t i = 0; i < n && 4u + (i + 1) * sizeof(GlueScene) <= e->size; ++i) {
                GlueScene s;
                std::memcpy(&s, b + 4 + i * sizeof(GlueScene), sizeof(GlueScene));
                s.name[sizeof(s.name) - 1] = '\0';
                if (std::strcmp(s.name, scene) == 0) {
                    scene_ = s;
                    found = true;
                    break;
                }
            }
            pp::pakFree(blob);
        }
    }
    char entry[32];
    std::snprintf(entry, sizeof(entry), "cs_%s", scene);
    const pp::PakEntry* e = found ? pak.find(entry) : nullptr;
    void* blob = e ? pak.load(e) : nullptr;
    if (blob && !pp::textureFromBlob(blob, e->size, tex_, true)) {
        pp::pakFree(blob);
        blob = nullptr;
    }
    pak.close();
    have_ = found && tex_.pixels;
    PSP_LOGI(pp::tag::Gfx, "glue_scene name=%s ok=%d kb=%u ms=%u", scene, have_ ? 1 : 0,
             static_cast<unsigned>(tex_.pixels ? pp::textureBytes(tex_) / 1024u : 0u),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    if (!have_ && tex_.pixels) pp::textureRelease(tex_);
}

bool GlueBackground::draw(UiRenderer& ui, float offsetX) const {
    if (!have_) return false;
    const float w = static_cast<float>(pp::kScreenW), h = static_cast<float>(pp::kScreenH);
    // PSP: the picture is one screen wide; moved right (character creation) it leaves a strip on the left. That
    // strip shows the picture's own left edge mirrored (was black: a black band behind the race column).
    if (offsetX > 0.0f) {
        const float edge = offsetX * static_cast<float>(tex_.uw) / w;
        ui.image(tex_, Rect{0, 0, offsetX, h}, SrcRect{edge, 0, -edge, static_cast<float>(tex_.uh)});
    }
    ui.image(tex_, Rect{offsetX, 0, w, h});
    return true;
}

} // namespace wowee::ui::psp
