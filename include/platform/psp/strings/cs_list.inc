// Every CLIENT string key the EBOOT uses - one file per area, so the batches that convert
// the areas in parallel never edit the same file. WOWEE_CS(KEY): KEY as strings.pak names it (GlobalStrings
// KEY, GlueStrings GLUE_KEY, DBC_CLASS_<id> / DBC_RACE_<id> / DBC_MAP_<id>). A key already in another area's
// file is not added again (the enum would have it twice): use it, or move it to cs_common.inc.
#include "platform/psp/strings/cs_common.inc"
#include "platform/psp/strings/cs_glue.inc"
#include "platform/psp/strings/cs_b1_social.inc"
#include "platform/psp/strings/cs_b2_guild_mail.inc"
#include "platform/psp/strings/cs_b3_items.inc"
#include "platform/psp/strings/cs_b4_groups.inc"
#include "platform/psp/strings/cs_b5_trade.inc"
#include "platform/psp/strings/cs_b6_npc_spells.inc"
#include "platform/psp/strings/cs_b7_world_hud.inc"
#include "platform/psp/strings/cs_b8_port.inc"
