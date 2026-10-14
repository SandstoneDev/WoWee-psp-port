// what the GameTooltip (src/ui/psp/ui_world_tooltip.cpp) reads of the session
// beyond the item cache, as upstream GameHandler keeps it:
//   - SMSG_SET_PROFICIENCY -> weaponProficiency_ / armorProficiency_ (game_handler_packets.cpp): the client
//     writes an item subclass the character cannot use in red;
//   - SMSG_BINDPOINTUPDATE -> homeBindZoneId_ (BindPointUpdateParser): the Hearthstone's "$z";
//   - the item template fields upstream's ItemQueryResponseParser reads past or skips (LockID,
//     RandomProperty / RandomSuffix, Block, MaxDurability, then RequiredDisenchantSkill, ArmorDamageModifier,
//     Duration, ItemLimitCategory): the answer is parsed by the unchanged upstream parser on a copy, and when
//     it ended where the 3.3.5a layout ends (five words after GemProperties) the extras are read at their
//     offsets from there. Nothing is consumed: handleInventoryPacket still takes the answer.
// PSP: upstream's "Your home has been set to X." chat line on a changed bind point is not written (English;
// the ruRU client has no such GlobalString - the server's own message says it).
#include "platform/psp/psp_world_session.hpp"

#include "platform/psp/psp_log.hpp"

#include <cstring>

namespace wowee::platform::psp {

bool WorldSession::handleTooltipPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::SMSG_SET_PROFICIENCY: {  // upstream: itemClass 2 -> weapons, 4 -> armor
            if (!packet.hasRemaining(5)) return true;
            const uint8_t itemClass = packet.readUInt8();
            const uint32_t mask = packet.readUInt32();
            if (itemClass == 2) weaponProficiency_ = mask;
            else if (itemClass == 4) armorProficiency_ = mask;
            if (++tipLogs_ <= 8)
                PSP_LOGI(tag::Net, "proficiency class=%u mask=%08X", static_cast<unsigned>(itemClass),
                         static_cast<unsigned>(mask));
            return true;
        }
        case LogicalOpcode::SMSG_BINDPOINTUPDATE: {
            game::BindPointUpdateData data;
            if (game::BindPointUpdateParser::parse(packet, data)) {
                homeBindMap_ = data.mapId;
                homeBindZone_ = data.zoneId;
                PSP_LOGI(tag::Net, "bind_point map=%u zone=%u", static_cast<unsigned>(data.mapId),
                         static_cast<unsigned>(data.zoneId));
            }
            return true;
        }
        case LogicalOpcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE: {
            // Peek: the same answer through upstream's parser on a copy.
            network::Packet copy = packet;
            copy.setReadPos(0);
            game::ItemQueryResponseData d;
            if (!game::ItemQueryResponseParser::parse(copy, d) || !d.valid || d.entry == 0) return false;
            // The parser stops after GemProperties; AzerothCore / TrinityCore 3.3.5a send five words more
            // (RequiredDisenchantSkill, ArmorDamageModifier, Duration, ItemLimitCategory, HolidayId). Behind
            // GemProperties: socket bonus, 3 x (colour, content), TotemCategory, BagFamily, Map, Area,
            // MaxDurability, ItemSet, Block, RandomSuffix, RandomProperty, Sheath, Material, LockID.
            const size_t pos = copy.getReadPos();
            if (pos < 80 || copy.getSize() - pos != 20) return false;
            const uint8_t* raw = copy.getData().data();
            auto word = [&](size_t at) {
                uint32_t v = 0;
                std::memcpy(&v, raw + at, 4);
                return v;
            };
            const size_t pre = pos - 80;  // LockID, the first of the 20 words before the end of the parse
            ItemTipExtra x;
            // Backwards from the description: PageText, LanguageID, PageMaterial, StartQuest follow it, the
            // bonding comes before it, and before that the five item spells (6 words each: id, trigger,
            // charges, cooldown, category, category cooldown).
            const size_t descLen = d.description.size() + 1;
            if (pre >= 16 + descLen + 4 + 120) {
                const size_t descStart = pre - 16 - descLen;
                const size_t spells = descStart - 4 - 120;
                for (int i = 0; i < 5; ++i) {
                    x.spellCharges[i] = static_cast<int32_t>(word(spells + i * 24 + 8));
                    x.spellCooldown[i] = static_cast<int32_t>(word(spells + i * 24 + 12));
                    x.spellCategory[i] = static_cast<int32_t>(word(spells + i * 24 + 16));
                    x.spellCategoryCooldown[i] = static_cast<int32_t>(word(spells + i * 24 + 20));
                }
            }
            x.lockId = word(pre);
            x.randomProperty = static_cast<int32_t>(word(pre + 12));
            x.randomSuffix = static_cast<int32_t>(word(pre + 16));
            x.block = word(pre + 20);
            x.maxDurability = word(pre + 28);
            x.totemCategory = word(pre + 44);  // Area, Map, BagFamily, TotemCategory after ItemSet
            const uint32_t admBits = word(pos + 4);
            std::memcpy(&x.armorDamageModifier, &admBits, 4);
            x.duration = word(pos + 8);
            x.limitCategory = word(pos + 12);
            itemTipExtra_[d.entry] = x;
            return false;
        }
        default:
            return false;
    }
}

const WorldSession::ItemTipExtra* WorldSession::itemTipExtra(uint32_t entry) const {
    auto it = itemTipExtra_.find(entry);
    return it == itemTipExtra_.end() ? nullptr : &it->second;
}

bool WorldSession::itemEnchantments(uint64_t guid, uint32_t& permanent, uint32_t& temporary,
                                    std::array<uint32_t, 3>& sockets) const {
    // an inspected player's slot (InspectFrame's GameTooltip:SetInventoryItem).
    if ((guid & 0xFFFF000000000000ull) == kInspectItemGuid) return inspectEnchantments(guid, permanent, temporary, sockets);
    auto it = onlineItems_.find(guid);
    if (guid == 0 || it == onlineItems_.end()) return false;
    permanent = it->second.permanentEnchantId;
    temporary = it->second.temporaryEnchantId;
    sockets = it->second.socketEnchantIds;
    return true;
}

}  // namespace wowee::platform::psp
