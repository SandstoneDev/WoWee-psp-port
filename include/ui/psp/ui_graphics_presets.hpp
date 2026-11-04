#pragma once
#include <algorithm>
#include <string_view>
namespace wowee::ui::psp {
enum GraphicsPreset { kGraphicsLow, kGraphicsMedium, kGraphicsHigh, kGraphicsUltra,
                      kGraphicsCustom, kGraphicsAdaptive };
inline bool graphicsQualityKey(std::string_view key) {
    constexpr std::string_view keys[]={"m2view","playerview","playermax","npcview","npcmax",
        "terrainsharp","sharpdist","classicfog","terraindetail","spellfx",
        "spellfxreal","levelupfx","brightness","water","particles","weather"};
    for(auto item:keys)if(item==key)return true;
    return false;
}
inline bool graphicsPresetAllows(int preset,std::string_view key) {
    return preset!=kGraphicsAdaptive || !graphicsQualityKey(key);
}
template<class Settings> void applyGraphicsPreset(Settings& s,int preset) {
    s.graphicspreset=std::clamp(preset,0,5);
    s.adaptive30=s.graphicspreset==kGraphicsAdaptive?1:0;
    if(s.graphicspreset==kGraphicsCustom)return; // retain the current picture
    struct Quality {int objects,players,playerCount,npcs,npcCount,sharp,sharpDistance,detail,spells,fancy,water,particles;};
    constexpr Quality levels[]={
        {80,60,5,40,12,0,60,0,1,0,0,0},
        {120,90,10,60,20,0,90,1,2,0,1,1},
        {160,120,15,80,32,1,120,1,2,1,1,1},
        {240,200,30,150,64,1,240,1,2,1,1,1},
    };
    // Adaptive starts at the existing safe distances/crowd limits. The frame
    // budget governor can lower and restore detail; spell/rain settings stay on.
    constexpr Quality adaptive{160,120,10,70,24,0,120,1,2,1,1,1};
    const auto& q=s.graphicspreset==kGraphicsAdaptive?adaptive:levels[s.graphicspreset];
    s.m2view=q.objects;s.playerview=q.players;s.playermax=q.playerCount;
    s.npcview=q.npcs;s.npcmax=q.npcCount;s.terrainsharp=q.sharp;s.sharpdist=q.sharpDistance;
    s.terraindetail=q.detail;s.spellfx=q.spells;s.spellfxreal=q.fancy;
    s.water=q.water;s.particles=q.particles;s.weather=1;s.levelupfx=1;
    s.classicfog=1;s.brightness=100;
    // FPS, controls, gameplay and audio preferences are independent of quality.
}
template<class Settings> void graphicsSettingEdited(Settings& s,std::string_view key) {
    if(graphicsQualityKey(key)){s.graphicspreset=kGraphicsCustom;s.adaptive30=0;}
}
} // namespace wowee::ui::psp
