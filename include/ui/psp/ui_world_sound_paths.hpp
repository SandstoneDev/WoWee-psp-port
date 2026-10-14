// Sounds. Every game path the ported sound managers ask for, in one
// place for both sides: the PSP (src/ui/psp/ui_world_sounds.cpp resolves them
// against data/sounds.pak) and the converter (tools/psp/converter
// psp_sound_conv bakes exactly these, plus SoundEntries rows).
//
// The lists are upstream's, verbatim: UiSoundManager::initialize,
// CombatSoundManager::initialize, ActivitySoundManager (initialize,
// exertionPrefixes, rebuild*ForProfile), MovementSoundManager::initialize, and
// audio/footstep_paths.hpp (included as is). Upstream keeps them inside the
// managers' .cpp files next to AssetManager reads; the PSP needs them without
// the managers (PSP: copied here, one copy, the same order - a probe list's
// order decides which clip "wins" where upstream plays library[0]).
#pragma once

#include "audio/footstep_paths.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::ui::psp {

// upstream UiSoundManager's play* functions, one id each.
enum class UiSound : uint8_t {
    BagOpen, BagClose, QuestLogOpen, QuestLogClose, CharacterSheetOpen, CharacterSheetClose,
    AuctionHouseOpen, AuctionHouseClose, GuildBankOpen, GuildBankClose,
    ButtonClick, MenuButtonClick,
    QuestActivate, QuestComplete, QuestFailed, QuestUpdate, FishingBite,
    LootCoinSmall, LootCoinLarge, LootItem,
    DropOnGround, PickupBag, PickupBook,
    Eating, Drinking, LevelUp, AchievementAlert,
    Error, TargetSelect, TargetDeselect, WhisperReceived, MailReceived, MinimapPing,
    Count
};

namespace sound_paths {

// UiSoundManager::initialize: the candidates of one sound in upstream's order
// (a later one is only tried when an earlier one is missing). Fallbacks to
// another sound (achievement -> level up, whisper -> select target, mail ->
// whisper, minimap ping -> select target) are the caller's (uiFallback).
inline std::vector<std::string> ui(UiSound s) {
    const std::string i = "Sound\\Interface\\";
    switch (s) {
        case UiSound::BagOpen: return {i + "iBackPackOpen.wav"};
        case UiSound::BagClose: return {i + "iBackPackClose.wav"};
        case UiSound::QuestLogOpen: return {i + "iQuestLogOpenA.wav"};
        case UiSound::QuestLogClose: return {i + "iQuestLogCloseA.wav"};
        case UiSound::CharacterSheetOpen: return {i + "iAbilitiesOpenA.wav"};
        case UiSound::CharacterSheetClose: return {i + "iAbilitiesCloseA.wav"};
        case UiSound::AuctionHouseOpen: return {i + "AuctionWindowOpen.wav"};
        case UiSound::AuctionHouseClose: return {i + "AuctionWindowClose.wav"};
        case UiSound::GuildBankOpen: return {i + "GuildVaultOpen.wav"};
        case UiSound::GuildBankClose: return {i + "GuildVaultClose.wav"};
        case UiSound::ButtonClick: return {i + "iUiInterfaceButtonA.wav"};
        case UiSound::MenuButtonClick: return {i + "iUIMainMenuButtonA.wav"};
        case UiSound::QuestActivate: return {i + "iQuestActivate.wav"};
        case UiSound::QuestComplete: return {i + "iQuestComplete.wav"};
        case UiSound::QuestFailed: return {i + "igQuestFailed.wav"};
        case UiSound::QuestUpdate: return {i + "iQuestUpdate.wav"};
        // Upstream loads three and plays library[0].
        case UiSound::FishingBite: return {"Sound\\Spell\\FishingBobber_Ver2_1.wav"};
        case UiSound::LootCoinSmall: return {i + "LootCoinSmall.wav"};
        case UiSound::LootCoinLarge: return {i + "LootCoinLarge.wav"};
        case UiSound::LootItem: return {i + "igLootCreature.wav"};
        case UiSound::DropOnGround: return {i + "DropOnGround.wav"};
        case UiSound::PickupBag: return {i + "PickUp\\PickUpBag.wav"};
        case UiSound::PickupBook: return {i + "PickUp\\PickUpBook.wav"};
        case UiSound::Eating: return {i + "iEating1.wav"};
        case UiSound::Drinking: return {i + "iDrinking1.wav"};
        case UiSound::LevelUp: return {i + "LevelUp.wav"};
        case UiSound::AchievementAlert: return {i + "AchievementSound.wav"};
        case UiSound::Error: return {i + "Error.wav"};
        case UiSound::TargetSelect: return {i + "iSelectTarget.wav"};
        case UiSound::TargetDeselect: return {i + "iDeselectTarget.wav"};
        case UiSound::WhisperReceived: return {i + "Whisper_TellMale.wav", i + "Whisper_TellFemale.wav"};
        case UiSound::MailReceived: return {i + "iTellMessage.wav"};
        case UiSound::MinimapPing: return {i + "MapPing.wav"};
        case UiSound::Count: break;
    }
    return {};
}

// Upstream's "if (!loadSound(...)) x_ = y_" fallbacks.
inline UiSound uiFallback(UiSound s) {
    switch (s) {
        case UiSound::AchievementAlert: return UiSound::LevelUp;
        case UiSound::WhisperReceived: return UiSound::TargetSelect;
        case UiSound::MailReceived: return UiSound::WhisperReceived;
        case UiSound::MinimapPing: return UiSound::TargetSelect;
        default: return UiSound::Count;
    }
}

// CombatSoundManager::initialize.
inline std::vector<std::string> swing(const char* size) {  // "Small", "Medium", "Large"
    const std::string w = std::string("Sound\\Item\\Weapons\\WeaponSwings\\mWoosh") + size;
    return {w + "1.wav", w + "2.wav", w + "3.wav"};
}
inline std::vector<std::string> swingCrit(const char* size) {
    return {std::string("Sound\\Item\\Weapons\\WeaponSwings\\mWoosh") + size + "Crit.wav"};
}
inline std::vector<std::string> missWhoosh(bool twoHanded) {
    return {twoHanded ? "Sound\\Item\\Weapons\\MissSwings\\MissWhoosh2Handed.wav"
                      : "Sound\\Item\\Weapons\\MissSwings\\MissWhoosh1Handed.wav"};
}
// Impacts, 1H axe as upstream's base: "Flesh", "Chain", "Plate",
// "MetalShield", "MetalWeapon" (one clip), "Wood", "Stone" (upper-case letters).
inline std::vector<std::string> impact(const char* material) {
    const std::string m(material);
    const std::string b = "Sound\\Item\\Weapons\\Axe1H\\m1hAxeHit" + m;
    if (m == "MetalWeapon") return {b + "1a.wav"};
    if (m == "Wood" || m == "Stone") return {b + "1A.wav", b + "1B.wav", b + "1C.wav"};
    return {b + "1a.wav", b + "1b.wav", b + "1c.wav"};
}
inline std::vector<std::string> impactCrit(const char* material) {  // Flesh, Chain, Plate, MetalShield
    return {std::string("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHit") + material + "Crit.wav"};
}

// ActivitySoundManager::initialize.
inline std::vector<std::string> splashEnter() {
    return {"Sound\\Character\\Footsteps\\EnterWaterSplash\\EnterWaterSmallA.wav",
            "Sound\\Character\\Footsteps\\EnterWaterSplash\\EnterWaterMediumA.wav",
            "Sound\\Character\\Footsteps\\EnterWaterSplash\\EnterWaterGiantA.wav",
            "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterA.wav",
            "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterB.wav",
            "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterC.wav",
            "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterD.wav",
            "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterE.wav"};
}
inline std::vector<std::string> meleeSwing() {
    return {"Sound\\Item\\Weapons\\WeaponSwings\\mWooshMedium1.wav",
            "Sound\\Item\\Weapons\\WeaponSwings\\mWooshMedium2.wav",
            "Sound\\Item\\Weapons\\WeaponSwings\\mWooshMedium3.wav",
            "Sound\\Item\\Weapons\\WeaponSwings\\mWooshLarge1.wav",
            "Sound\\Item\\Weapons\\WeaponSwings\\mWooshLarge2.wav",
            "Sound\\Item\\Weapons\\WeaponSwings\\mWooshLarge3.wav",
            "Sound\\Item\\Weapons\\MissSwings\\MissWhoosh1Handed.wav",
            "Sound\\Item\\Weapons\\MissSwings\\MissWhoosh2Handed.wav"};
}

// ActivitySoundManager's exertionPrefixes (a static function upstream).
inline std::vector<std::string> exertionPrefixes(const std::string& raceBase, bool male) {
    const std::string gender = male ? "Male" : "Female";
    const std::string race = (raceBase == "Scourge") ? "Undead" : raceBase;
    const std::string stem = race + gender;
    const std::string base = "Sound\\Character\\PlayerExertions\\";
    std::vector<std::string> prefixes;
    prefixes.push_back(base + stem + "Final\\" + stem + "Main");
    prefixes.push_back(base + stem + "\\" + stem + "Main");
    prefixes.push_back(base + stem + "Final\\" + stem + "Final");
    if (race == "Human" && !male) prefixes.push_back(base + stem + "Final\\HumanFeamleMain");
    if (race == "Troll" && !male) prefixes.push_back(base + stem + "Final\\TrollFemalMain");
    return prefixes;
}

// rebuildJumpClipsForProfile.
inline std::vector<std::string> jump(const std::string& raceFolder, const std::string& raceBase, bool male) {
    const std::string gender = male ? "Male" : "Female";
    const std::string prefix = "Sound\\Character\\" + raceFolder + "\\";
    const std::string stem = raceBase + gender;
    std::vector<std::string> c;
    for (const auto& exert : exertionPrefixes(raceBase, male)) c.push_back(exert + "Jump.wav");
    c.push_back(prefix + stem + "Jump1.wav");
    c.push_back(prefix + stem + "Land1.wav");
    c.push_back(prefix + stem + "JumpA.wav");
    c.push_back(prefix + stem + "JumpB.wav");
    c.push_back(prefix + stem + "Jump.wav");
    c.push_back(prefix + gender + "\\" + stem + "JumpA.wav");
    c.push_back(prefix + gender + "\\" + stem + "JumpB.wav");
    c.push_back(prefix + stem + "\\" + stem + "Jump01.wav");
    c.push_back(prefix + stem + "\\" + stem + "Jump02.wav");
    return c;
}

// rebuildSwimLoopClipsForProfile (race independent).
inline std::vector<std::string> swimStroke() {
    std::vector<std::string> c;
    for (const char* size : {"Medium", "Small"})
        for (char l = 'A'; l <= 'E'; ++l)
            c.push_back(std::string("Sound\\Character\\Footsteps\\WaterSplash\\FootSteps") + size + "Water" + l + ".wav");
    return c;
}

// rebuildHardLandClipsForProfile.
inline std::vector<std::string> hardLand(const std::string& raceFolder, const std::string& raceBase, bool male) {
    const std::string gender = male ? "Male" : "Female";
    const std::string prefix = "Sound\\Character\\" + raceFolder + "\\";
    const std::string stem = raceBase + gender;
    return {prefix + stem + "\\" + stem + "LandHard01.wav", prefix + stem + "\\" + stem + "LandHard02.wav",
            prefix + stem + "LandHard01.wav", prefix + stem + "LandHard02.wav"};
}

// rebuildCombatVocalClipsForProfile: kind 0 attack, 1 wound, 2 wound crit, 3 death.
inline std::vector<std::string> vocals(int kind, const std::string& raceFolder, const std::string& raceBase,
                                       bool male) {
    const std::string gender = male ? "Male" : "Female";
    const std::string stem = raceBase + gender;
    const std::vector<std::string> ex = exertionPrefixes(raceBase, male);
    const std::string vox = "Sound\\Character\\" + raceFolder + "\\" + gender + "\\m" + stem;
    std::vector<std::string> p;
    if (kind == 0) {
        for (char c = 'A'; c <= 'F'; ++c) {
            const std::string s(1, c);
            for (const auto& e : ex) p.push_back(e + "Attack" + s + ".wav");
            p.push_back(vox + "AttackVox" + s + ".wav");
        }
    } else if (kind == 1) {
        for (char c = 'A'; c <= 'F'; ++c) {
            const std::string s(1, c);
            for (const auto& e : ex) p.push_back(e + "Wound" + s + ".wav");
            p.push_back(vox + "WoundVox" + s + ".wav");
        }
        for (const auto& e : ex) p.push_back(e + "Wound.wav");
    } else if (kind == 2) {
        for (char c = 'A'; c <= 'C'; ++c) {
            const std::string s(1, c);
            for (const auto& e : ex) {
                p.push_back(e + "WoundCrit" + s + ".wav");
                p.push_back(e + "WoundCriat" + s + ".wav");
            }
            p.push_back(vox + "WoundCriticalVox" + s + ".wav");
        }
        for (const auto& e : ex) p.push_back(e + "WoundCrit.wav");
    } else {
        for (char c = 'A'; c <= 'C'; ++c) {
            const std::string s(1, c);
            for (const auto& e : ex) p.push_back(e + "Death" + s + ".wav");
            p.push_back(vox + "DeathVox" + s + ".wav");
        }
        for (const auto& e : ex) p.push_back(e + "Death.wav");
    }
    return p;
}

// MovementSoundManager::initialize.
inline std::vector<std::string> enterWater(const char* size) {  // "Small", "Medium", "Giant"
    return {std::string("Sound\\Spells\\EnterWater") + size + ".wav"};
}
inline std::vector<std::string> waterFootstep(bool huge) {
    std::vector<std::string> c;
    for (char l = 'A'; l <= 'E'; ++l)
        c.push_back(huge ? std::string("Sound\\Character\\Footsteps\\FootStepsHugeWater") + l + ".wav"
                         : std::string("Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWater") + l + ".wav");
    return c;
}

// Races of ActivitySoundManager::setCharacterVoiceProfile (folder, base) by
// ChrRaces id (the PSP knows the id, not the model name). nullptr = none.
struct RaceVoice {
    const char* folder;
    const char* base;
};
inline RaceVoice raceVoice(uint32_t raceId) {
    switch (raceId) {
        case 1: return {"Human", "Human"};
        case 2: return {"Orc", "Orc"};
        case 3: return {"Dwarf", "Dwarf"};
        case 4: return {"NightElf", "NightElf"};
        case 5: return {"Scourge", "Scourge"};
        case 6: return {"Tauren", "Tauren"};
        case 7: return {"Gnome", "Gnome"};
        case 8: return {"Troll", "Troll"};
        case 9: return {"Goblin", "Goblin"};
        case 10: return {"BloodElf", "BloodElf"};
        case 11: return {"Draenei", "Draenei"};
        default: return {"Human", "Human"};  // upstream's default profile
    }
}

// Every path above for every race and sex (the converter's bake list).
inline std::vector<std::string> all() {
    std::vector<std::string> out;
    auto add = [&](const std::vector<std::string>& v) { out.insert(out.end(), v.begin(), v.end()); };
    for (int s = 0; s < static_cast<int>(UiSound::Count); ++s) add(ui(static_cast<UiSound>(s)));
    for (const char* sz : {"Small", "Medium", "Large"}) {
        add(swing(sz));
        add(swingCrit(sz));
    }
    add(missWhoosh(false));
    add(missWhoosh(true));
    for (const char* m : {"Flesh", "Chain", "Plate", "MetalShield", "MetalWeapon", "Wood", "Stone"}) add(impact(m));
    for (const char* m : {"Flesh", "Chain", "Plate", "MetalShield"}) add(impactCrit(m));
    add(splashEnter());
    add(meleeSwing());
    add(swimStroke());
    for (uint32_t race : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u}) {
        const RaceVoice rv = raceVoice(race);
        for (bool male : {true, false}) {
            add(jump(rv.folder, rv.base, male));
            add(hardLand(rv.folder, rv.base, male));
            for (int k = 0; k < 4; ++k) add(vocals(k, rv.folder, rv.base, male));
        }
    }
    for (const char* sz : {"Small", "Medium", "Giant"}) add(enterWater(sz));
    add(waterFootstep(false));
    add(waterFootstep(true));
    // FootstepManager::initialize (audio/footstep_paths.hpp, upstream's own).
    for (const char* m : {"Stone", "Dirt", "Grass", "Wood", "Snow", "Metal"}) {
        add(audio::classicFootstepPaths(m));
        add(audio::horseFootstepPaths(m));
        add(audio::hugeFootstepPaths(m));
    }
    add(audio::waterFootstepPaths());
    add(audio::hugeWaterFootstepPaths());
    add(audio::altFootstepPaths("MediumLargeMetalFootsteps", "MediumLargeFootstepMetal"));
    return out;
}

} // namespace sound_paths
} // namespace wowee::ui::psp
