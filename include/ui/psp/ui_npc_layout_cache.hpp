#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>
namespace wowee::ui::psp {
// Scroll/focus/selection are live draw state, not cache keys.
struct NpcLayoutKey {
    uint64_t character=0,npc=0;
    uint32_t dialog=0,fonts=0,npcNames=0;
    uint8_t race=0,cls=0,gender=2;
    float scale=1;
    std::string playerName,locale;
    bool operator==(const NpcLayoutKey& b) const {
        return std::tie(character,npc,dialog,fonts,npcNames,race,cls,gender,scale,playerName,locale)==
               std::tie(b.character,b.npc,b.dialog,b.fonts,b.npcNames,b.race,b.cls,b.gender,b.scale,b.playerName,b.locale);
    }
};
struct NpcFrameLayout {
    struct Line {
        enum Kind : uint8_t { Text,Head,Row,Item,Money,Gap } kind;
        std::string s;
        uint32_t col=0,id=0;
        uint8_t icon=0;
        uint32_t value=0,display=0;
        int choice=-1;
        float h=0,y=0;
        uint32_t itemId=0;
        bool right=false;
        std::vector<std::string> labels{}; // item name lines or three coin amounts
        std::array<float,3> widths{};
        std::string count{};
    };
    NpcLayoutKey key;
    bool valid=false,nameSmall=false;
    std::string name;
    float nameWidth=0,total=0;
    std::vector<Line> lines;
    std::array<std::string,2> buttons;
    std::array<float,2> buttonWidths{};
    uint32_t builds=0,hits=0;
};
}
