# WoWee PSP port - separate CMake project.
#
# This is deliberately NOT wired into the top-level CMakeLists.txt: the desktop
# build stays byte-identical to upstream, and upstream merges never conflict
# here. The PSP project reuses upstream sources by path, unchanged.
#
#   export PSPDEV=$HOME/pspdev
#   cmake -S psp -B build-psp -DCMAKE_TOOLCHAIN_FILE=cmake/psp-toolchain.cmake \
#         -DCMAKE_BUILD_TYPE=Release -DWOWEE_PSP_BUILD=<n>
#   cmake --build build-psp
#
# tools/psp/build_psp.sh does exactly this inside WSL (see docs/PSP_BUILD.md).

cmake_minimum_required(VERSION 3.20)

project(wowee_psp C CXX)

# The toolchain file is read by project(), so PSP is only known after it.
if(NOT PSP)
    message(FATAL_ERROR "The PSP project must be configured with -DCMAKE_TOOLCHAIN_FILE=cmake/psp-toolchain.cmake")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# gnu++20 like upstream (CMake default): upstream code uses POSIX calls
# (setenv, strdup...) that strict -std=c++20 hides.
set(CMAKE_CXX_EXTENSIONS ON)

get_filename_component(WOWEE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Build number: printed in the session.log header, so the log proves which
# build is on the card. tools/psp/build_psp.sh passes psp/BUILD_NUMBER.
set(WOWEE_PSP_BUILD "0" CACHE STRING "PSP build number (bumped on every deploy)")
set(WOWEE_PSP_RELEASE "0.1.0")

# ---------------------------------------------------------------------------
# Upstream sources reused unchanged. This list grows milestone by milestone;
# each entry must compile for the PSP with no edit to the upstream file.
# ---------------------------------------------------------------------------
set(WOWEE_PSP_UPSTREAM_SOURCES
    ${WOWEE_ROOT}/src/network/packet.cpp
    ${WOWEE_ROOT}/src/network/socket.cpp
    ${WOWEE_ROOT}/src/network/tcp_socket.cpp
    ${WOWEE_ROOT}/src/math/spline.cpp
    # Auth layer (same set as upstream's auth_login_probe tool, plus the handler).
    ${WOWEE_ROOT}/src/auth/auth_handler.cpp
    ${WOWEE_ROOT}/src/auth/auth_packets.cpp
    ${WOWEE_ROOT}/src/auth/auth_opcodes.cpp
    ${WOWEE_ROOT}/src/auth/big_num.cpp
    ${WOWEE_ROOT}/src/auth/crypto.cpp
    ${WOWEE_ROOT}/src/auth/integrity.cpp
    ${WOWEE_ROOT}/src/auth/pin_auth.cpp
    ${WOWEE_ROOT}/src/auth/srp.cpp
    ${WOWEE_ROOT}/src/auth/rc4.cpp
    ${WOWEE_ROOT}/src/auth/vanilla_crypt.cpp
    # World connection + packet layer (the set upstream's packet tests build).
    ${WOWEE_ROOT}/src/network/world_socket.cpp
    ${WOWEE_ROOT}/src/game/character.cpp
    ${WOWEE_ROOT}/src/game/expansion_profile.cpp
    ${WOWEE_ROOT}/src/game/opcode_table.cpp
    ${WOWEE_ROOT}/src/game/packet_parsers_classic.cpp
    ${WOWEE_ROOT}/src/game/packet_parsers_tbc.cpp
    ${WOWEE_ROOT}/src/game/spline_packet.cpp
    ${WOWEE_ROOT}/src/game/world_packets.cpp
    ${WOWEE_ROOT}/src/game/world_packets_economy.cpp
    ${WOWEE_ROOT}/src/game/world_packets_entity.cpp
    ${WOWEE_ROOT}/src/game/entity.cpp
    ${WOWEE_ROOT}/src/game/update_field_table.cpp
    ${WOWEE_ROOT}/src/game/world_packets_social.cpp
    ${WOWEE_ROOT}/src/game/world_packets_world.cpp
    # M9: the player's movement animation state machine (pure logic).
    ${WOWEE_ROOT}/src/rendering/animation/locomotion_fsm.cpp
    ${WOWEE_ROOT}/src/rendering/animation/character_animator.cpp
    ${WOWEE_ROOT}/src/rendering/animation/activity_fsm.cpp
    ${WOWEE_ROOT}/src/rendering/animation/combat_fsm.cpp
    ${WOWEE_ROOT}/src/rendering/animation/mount_fsm.cpp
    ${WOWEE_ROOT}/src/game/text_tokens.cpp  # Chat ($N/$g in NPC lines)
    ${WOWEE_ROOT}/src/game/inventory.cpp  # Inventory (game::Inventory model)
    ${WOWEE_ROOT}/src/rendering/zone_ambience.cpp  # Lighting (upstream zone light overrides)
    ${WOWEE_ROOT}/src/rendering/world_map/coordinate_projection.cpp  # Map (world map logic, unchanged)
    ${WOWEE_ROOT}/src/game/warden_crypto.cpp  # PSP: Warden's SHA1Randx + RC4, unchanged
    ${WOWEE_ROOT}/src/rendering/world_map/exploration_state.cpp      # Map
    ${WOWEE_ROOT}/src/rendering/world_map/view_state_machine.cpp     # Map
)

# ---------------------------------------------------------------------------
# PSP platform layer (new code, lives only on psp-port).
# ---------------------------------------------------------------------------
set(WOWEE_PSP_PLATFORM_SOURCES
    ${WOWEE_ROOT}/src/platform/psp/main_psp.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_log.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_system.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_fs.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_locale.cpp   # The client's languages (data/locale/<xxYY>/)
    ${WOWEE_ROOT}/src/platform/psp/psp_strings.cpp  # Interface strings (strings.pak + own en/ru)
    ${WOWEE_ROOT}/src/platform/psp/psp_input.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_display.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_audio.cpp
    ${WOWEE_ROOT}/src/platform/psp/reuse_probe.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_pak.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_pak_index.cpp  # big sorted paks, TOC on the card
    ${WOWEE_ROOT}/src/platform/psp/psp_texture.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_prof.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_net.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_config.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_autotest.cpp  # The on-device autopilot (autotest.txt)
    # PSP stand-ins for upstream pieces with desktop-only dependencies.
    ${WOWEE_ROOT}/src/platform/psp/psp_core_logger.cpp       # replaces src/core/logger.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_openssl_compat.cpp    # OpenSSL subset on mbedTLS
    ${WOWEE_ROOT}/src/platform/psp/psp_socket_wrap.cpp       # connect() errno mapping (--wrap)
    ${WOWEE_ROOT}/src/platform/psp/psp_public_safe.cpp       # PSP: debug / automation aids only on a dev server
    ${WOWEE_ROOT}/src/platform/psp/psp_warden_core.cpp       # PSP: Warden without the x86 module (portable)
    ${WOWEE_ROOT}/src/platform/psp/psp_warden.cpp            # PSP: Warden on the world session (card files)
    ${WOWEE_ROOT}/src/platform/psp/psp_integrity_core.cpp    # PSP: logon integrity hash (portable)
    ${WOWEE_ROOT}/src/platform/psp/psp_integrity.cpp         # PSP: logon integrity hash from the card
    ${WOWEE_ROOT}/src/platform/psp/psp_bigheap.cpp           # big-block heap region (--wrap=malloc...)
    ${WOWEE_ROOT}/src/platform/psp/psp_application.cpp       # backs the shadowed core::Application
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_combat.cpp  # Combat
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_death.cpp   # Ghost, corpse, resurrection, teleport
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_spells.cpp  # spells
    ${WOWEE_ROOT}/src/platform/psp/psp_spell_db.cpp              # spells: data/spells.pak
    ${WOWEE_ROOT}/src/platform/psp/psp_item_icons.cpp             # itemicons.pak
    ${WOWEE_ROOT}/src/platform/psp/psp_vmath.cpp                 # VFPU math bench (psp_vmath.hpp)
    # SandstoneDebug (source-psp's crash catcher, psp/sandstone-debug): the
    # user half that loads sdbg.prx and keeps the log ring.
    ${WOWEE_ROOT}/psp/sandstone-debug/lib/sdbg.c
    ${WOWEE_ROOT}/psp/sandstone-debug/lib/sdbg_psplink.c
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_move.cpp  # Creature anim (SMSG_MONSTER_MOVE)
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_mount.cpp  # Mounts (mount field, speed changes)
    ${WOWEE_ROOT}/src/platform/psp/psp_osk.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_netconf.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_login_store.cpp  # PSP (owner 14.10): the saved login outside the game folder
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_chat.cpp  # Chat
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_loot.cpp  # Loot
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_npc.cpp  # NPC
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_factions.cpp  # Factions
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_inventory.cpp  # Inventory
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_time.cpp  # Lighting (realm clock)
    ${WOWEE_ROOT}/src/platform/psp/psp_sound_bank.cpp  # Sounds
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_map.cpp  # Map (explored zones)
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_tooltip.cpp  # item extras, proficiency, bind point
)

# ---------------------------------------------------------------------------
# PSP UI (new code): renderer, fonts, focus ring, screens.
# ---------------------------------------------------------------------------
set(WOWEE_PSP_UI_SOURCES
    ${WOWEE_ROOT}/src/ui/psp/ui_font.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_renderer.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_focus.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_kit.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_art.cpp                     # Window art with a failure trail
    ${WOWEE_ROOT}/src/ui/psp/ui_login_screen.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_character_screen.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_glue.cpp            # Glue screens (GlueDialog, art, strings)
    ${WOWEE_ROOT}/src/ui/psp/ui_language.cpp        # The language selector (LanguageFrame)
    ${WOWEE_ROOT}/src/ui/psp/ui_glue_model.cpp      # The character model on CharacterSelect
    ${WOWEE_ROOT}/src/ui/psp/ui_glue_scene.cpp      # character-select backgrounds by race
    ${WOWEE_ROOT}/src/ui/psp/ui_character_create.cpp  # CharacterCreate as the original
    ${WOWEE_ROOT}/src/ui/psp/ui_loading_screen.cpp  # LoadingScreens.dbc + LoadingBar
    ${WOWEE_ROOT}/src/ui/psp/ui_world_screen.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_creatures.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_mount.cpp  # Mounts
    ${WOWEE_ROOT}/src/ui/psp/ui_world_windows.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_audio.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_streaming.cpp  # Streaming
    ${WOWEE_ROOT}/src/ui/psp/ui_world_target.cpp   # Combat
    ${WOWEE_ROOT}/src/ui/psp/ui_world_combat.cpp   # Combat
    ${WOWEE_ROOT}/src/ui/psp/ui_world_death.cpp    # Death popups (StaticPopup DEATH / RECOVER_CORPSE / RESURRECT / XP_LOSS)
    ${WOWEE_ROOT}/src/ui/psp/ui_world_spells.cpp   # spells
    ${WOWEE_ROOT}/src/ui/psp/ui_world_mainmenubar.cpp  # MainMenuBar, XP bar, usable buttons
    ${WOWEE_ROOT}/src/ui/psp/ui_world_tooltip.cpp      # GameTooltip / ShoppingTooltip, spell tooltips
    ${WOWEE_ROOT}/src/ui/psp/ui_world_controls.cpp # controls with modes (docs/PSP_CONTROLS.md)
    ${WOWEE_ROOT}/src/ui/psp/ui_settings_menu.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_prof_overlay.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_autotest_overlay.cpp  # The autopilot's band and summary
    ${WOWEE_ROOT}/src/ui/psp/ui_world_chat.cpp  # Chat
    ${WOWEE_ROOT}/src/ui/psp/ui_world_errors.cpp  # UIErrorsFrame
    ${WOWEE_ROOT}/src/ui/psp/ui_world_loot.cpp  # Loot
    ${WOWEE_ROOT}/src/ui/psp/ui_world_npc.cpp  # NPC
    ${WOWEE_ROOT}/src/ui/psp/ui_world_npc_frame.cpp               # GossipFrame / QuestFrame
    ${WOWEE_ROOT}/src/ui/psp/ui_world_merchant.cpp                # MerchantFrame
    ${WOWEE_ROOT}/src/ui/psp/ui_world_questlog.cpp                # QuestLogFrame
    ${WOWEE_ROOT}/src/ui/psp/ui_world_bags.cpp                    # ContainerFrame
    ${WOWEE_ROOT}/src/ui/psp/ui_world_paperdoll.cpp               # CharacterFrame / PaperDollFrame
    ${WOWEE_ROOT}/src/ui/psp/ui_world_char_tabs.cpp               # ReputationFrame / SkillFrame
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_chartabs.cpp # their session data
    ${WOWEE_ROOT}/src/ui/psp/ui_world_dressup.cpp                 # DressUpFrame, the paperdoll's check boxes
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_dressup.cpp  # ShowHelm / ShowCloak
    ${WOWEE_ROOT}/src/ui/psp/ui_world_party.cpp                   # PartyMemberFrame, UnitPopup, PARTY_INVITE
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_party.cpp    # the group (SocialHandler's party half)
    ${WOWEE_ROOT}/src/ui/psp/ui_world_pets.cpp                    # PetFrame, PetActionBar, PetStableFrame
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_pets.cpp     # the pet, its talents, the stable
    ${WOWEE_ROOT}/src/ui/psp/ui_world_classbars.cpp               # the class bars, the vehicles
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_classbars.cpp # totems, forms, possession, vehicles
    ${WOWEE_ROOT}/src/ui/psp/ui_world_watch.cpp                   # WatchFrame
    ${WOWEE_ROOT}/src/ui/psp/ui_world_spellbook.cpp               # SpellBookFrame
    ${WOWEE_ROOT}/src/ui/psp/ui_world_popup.cpp                   # StaticPopup, GameMenuFrame
    ${WOWEE_ROOT}/src/ui/psp/ui_world_inventory.cpp  # Inventory windows
    ${WOWEE_ROOT}/src/ui/psp/fx_runtime.cpp        # FrameXML
    ${WOWEE_ROOT}/src/ui/psp/ui_framexml.cpp       # FrameXML
    ${WOWEE_ROOT}/src/ui/psp/ui_world_framexml.cpp # FrameXML
    ${WOWEE_ROOT}/src/ui/psp/ui_layers.cpp         # Layer manager
    # PSP renderer (GE)
    ${WOWEE_ROOT}/src/rendering/psp/psp_terrain.cpp
    ${WOWEE_ROOT}/src/rendering/psp/psp_models.cpp
    ${WOWEE_ROOT}/src/rendering/psp/psp_impostors.cpp  # Forest LOD
    ${WOWEE_ROOT}/src/rendering/psp/psp_character.cpp
    ${WOWEE_ROOT}/src/rendering/psp/psp_skinned.cpp  # Creature anim (GE skinning per instance)
    ${WOWEE_ROOT}/src/rendering/psp/psp_equipment.cpp
    ${WOWEE_ROOT}/src/rendering/psp/psp_equipment_dress.cpp   # clean room: equipment_dress.md
    ${WOWEE_ROOT}/src/rendering/psp/psp_weapon_placement.cpp  # clean room: weapon_placement.md / weapon_glow.md
    ${WOWEE_ROOT}/src/rendering/psp/psp_creatures.cpp
    ${WOWEE_ROOT}/src/rendering/psp/psp_attachments.cpp
    ${WOWEE_ROOT}/src/rendering/psp/psp_spellfx.cpp  # spell effects (spellfx.pak)
    ${WOWEE_ROOT}/src/ui/psp/ui_world_spellfx.cpp    # their world screen half
    ${WOWEE_ROOT}/src/rendering/psp/psp_particles.cpp  # particle engine, world emitters, weather
    ${WOWEE_ROOT}/src/rendering/psp/psp_particle_sim.cpp  # level-3 particle rules (clean-room)
    ${WOWEE_ROOT}/src/rendering/psp/psp_particles_l3.cpp  # the particle engine's level-3 path (clean-room)
    ${WOWEE_ROOT}/src/rendering/psp/psp_particles_l3_gu.cpp  # its GE output (sceGu)
    ${WOWEE_ROOT}/src/ui/psp/ui_world_particles.cpp    # their world screen half
    ${WOWEE_ROOT}/src/ui/psp/ui_world_weather.cpp      # SMSG_WEATHER -> the weather
    ${WOWEE_ROOT}/src/ui/psp/ui_world_sheath.cpp  # Weapons sheathed out of combat
    ${WOWEE_ROOT}/src/ui/psp/ui_world_weapons.cpp       # sheath types, glows (world screen half)
    ${WOWEE_ROOT}/src/rendering/psp/psp_weapon_fx.cpp   # the glows on weapons
    ${WOWEE_ROOT}/src/ui/psp/ui_world_hud.cpp  # HUD layer order (until task A)
    ${WOWEE_ROOT}/src/rendering/psp/psp_lighting.cpp     # Lighting (LightingManager logic)
    ${WOWEE_ROOT}/src/rendering/psp/psp_lighting_ge.cpp  # Lighting (fog, tint, sky, glow)
    ${WOWEE_ROOT}/src/ui/psp/ui_world_lighting.cpp       # Lighting
    ${WOWEE_ROOT}/src/ui/psp/ui_world_sounds.cpp  # Sounds
    ${WOWEE_ROOT}/src/rendering/psp/psp_footsteps.cpp  # Sounds ($FSD accessors)
    # Trainers + talents + spell book tabs
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_trainer.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_talents.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_spell_db_talents.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_trainer.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_tradeskill.cpp                # TradeSkillFrame
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_tradeskill.cpp # crafting window, craft.pak
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_tutorial.cpp  # Tutorials (flags, triggers)
    ${WOWEE_ROOT}/src/platform/psp/psp_tutorial_db.cpp             # Tutorials (data/tutorial.pak)
    ${WOWEE_ROOT}/src/ui/psp/ui_world_tutorial.cpp                 # Tutorials (TutorialFrame)
    ${WOWEE_ROOT}/src/ui/psp/ui_world_minimap.cpp  # Map (minimap, minimap_<map>.pak)
    ${WOWEE_ROOT}/src/ui/psp/ui_world_map.cpp      # Map (world map window)
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_minimap.cpp  # mail / calendar notices
    ${WOWEE_ROOT}/src/ui/psp/fx_runtime_game.cpp          # FrameXML - action bar, cast, auras, book, bags API
    ${WOWEE_ROOT}/src/ui/psp/ui_world_framexml_game.cpp   # FrameXML - the adapter half over WorldSession
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_taxi.cpp  # flight paths (session, data/taxi.pak)
    ${WOWEE_ROOT}/src/ui/psp/ui_world_taxi.cpp                 # TaxiFrame, the flight, the landing
    # BankFrame, MailFrame / OpenMailFrame and their session half
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_mail.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_gameobject.cpp  # Game objects in sight
    ${WOWEE_ROOT}/src/ui/psp/ui_world_gameobjects.cpp               # Their models
    ${WOWEE_ROOT}/src/ui/psp/ui_world_bank.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_mail.cpp
    # AuctionFrame and the session's auction half
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_auction.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_auction.cpp
    # transfers between maps, area triggers, instance messages and dialogs
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_transfer.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_instance.cpp
    # the PMF movie player (sceMpeg), MovieFrame / CinematicsFrame, the session's movies
    ${WOWEE_ROOT}/src/platform/psp/psp_movie.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_movie.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_movie.cpp
    # the raid and the battlegrounds (session half, RaidFrame / BattlefieldFrame / world states)
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_raid.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_battleground.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_raid.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_battleground.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_worldstate.cpp
    # the Dungeon Finder, the achievements, the calendar (their session half, data/wotlk.pak)
    ${WOWEE_ROOT}/src/game/calendar_data.cpp  # upstream: SMSG_CALENDAR_SEND_CALENDAR / _SEND_EVENT parsers
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_lfg.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_achievements.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_calendar.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_lfd.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_achievements.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_calendar.cpp
    # the inspect (InspectFrame and the session's inspect half)
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_inspect.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_inspect.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_delete_item.cpp             # StaticPopup DELETE_ITEM
    ${WOWEE_ROOT}/src/ui/psp/ui_world_devtools.cpp                # Test aids (gmtools / devcmd)
    # the NPC races' helm models (items.pak "hr_<display>")
    ${WOWEE_ROOT}/src/rendering/psp/psp_item_npc_race.cpp
    # big WMOs (data/bigwmo/*.pak, the doodads pak's "wbig")
    ${WOWEE_ROOT}/src/rendering/psp/psp_bigwmo.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_bigwmo.cpp
    # WMO game objects, lifts, ships and zeppelins (the client's motion), riding them
    ${WOWEE_ROOT}/src/rendering/psp/psp_transport_path.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_transport.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_transports.cpp
    # flight and swimming in 3D (the server's flag toggles + ACKs, take-off / landing, the swim check)
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_flying.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_flying.cpp
    # using game objects (the session half, the hover / tooltip, ItemTextFrame, BarberShopFrame)
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_gouse.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_gouse.cpp
    # using items (CMSG_USE_ITEM / CMSG_OPEN_ITEM, item cooldowns, item buttons, SpellCastTargets)
    ${WOWEE_ROOT}/src/platform/psp/psp_spell_targets.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_items.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_item_use.cpp
    # friends / ignore / who / guild (FriendsFrame tabs, GuildFrame and its side frames, the session half)
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_social.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_social.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_guild.cpp
    # TradeFrame, the duels, the request popups (session half + world screen half)
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_trade_duel.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_trade_duel.cpp
    # GlyphFrame, GuildBankFrame, ChannelFrame (session half + world screen half)
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_glyphs.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_guildbank.cpp
    ${WOWEE_ROOT}/src/platform/psp/psp_world_session_channels.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_glyphs.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_guildbank.cpp
    ${WOWEE_ROOT}/src/ui/psp/ui_world_channels.cpp
    # the memory governor, attachcap's crowd queue with the NPCs, the descriptor budget
    ${WOWEE_ROOT}/src/ui/psp/ui_world_memory.cpp
)

# Lua 5.1.5 of upstream (extern/lua-5.1.5, with its lvm.c change) as
# its own static library. PSP: the sources are copied into the build tree next
# to src/platform/psp/lua/luaconf.h (float lua_Number, GC 110/800,
# LUAI_MAXCCALLS 120) - a luaconf.h beside the .c files always wins over one
# on the include path (docs/PSP_LUA_NOTES.md). No io/os/package/debug: the
# runtime opens base, table, string and math (upstream LuaEngine::initialize).
set(WOWEE_LUA_DIR ${CMAKE_CURRENT_BINARY_DIR}/lua51)
set(WOWEE_LUA_CORE lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring
    ltable ltm lundump lvm lzio lauxlib lbaselib lmathlib lstrlib ltablib)
set(WOWEE_LUA_SOURCES)
file(GLOB WOWEE_LUA_HEADERS ${WOWEE_ROOT}/extern/lua-5.1.5/src/*.h)
foreach(h ${WOWEE_LUA_HEADERS})
    get_filename_component(hn ${h} NAME)
    if(NOT hn STREQUAL "luaconf.h")
        configure_file(${h} ${WOWEE_LUA_DIR}/${hn} COPYONLY)
    endif()
endforeach()
configure_file(${WOWEE_ROOT}/src/platform/psp/lua/luaconf.h ${WOWEE_LUA_DIR}/luaconf.h COPYONLY)
foreach(f ${WOWEE_LUA_CORE})
    configure_file(${WOWEE_ROOT}/extern/lua-5.1.5/src/${f}.c ${WOWEE_LUA_DIR}/${f}.c COPYONLY)
    list(APPEND WOWEE_LUA_SOURCES ${WOWEE_LUA_DIR}/${f}.c)
endforeach()
add_library(wowee_lua51 STATIC ${WOWEE_LUA_SOURCES})
target_include_directories(wowee_lua51 PUBLIC ${WOWEE_LUA_DIR})
target_compile_options(wowee_lua51 PRIVATE -G0 -O2)

# The community QR splash at start (src/platform/psp/psp_splash.cpp). A build switch only: OFF leaves its source
# and its call in main() out of the EBOOT; there is no run-time setting for it.
option(WOWEE_PSP_SPLASH "Show the community QR splash at start" ON)
if(WOWEE_PSP_SPLASH)
    list(APPEND WOWEE_PSP_PLATFORM_SOURCES ${WOWEE_ROOT}/src/platform/psp/psp_splash.cpp)
endif()

add_executable(wowee_psp
    ${WOWEE_PSP_PLATFORM_SOURCES}
    ${WOWEE_PSP_UI_SOURCES}
    ${WOWEE_PSP_UPSTREAM_SOURCES}
)

target_include_directories(wowee_psp PRIVATE
    ${WOWEE_ROOT}/psp/sandstone-debug/lib  # sdbg.h (crash catcher)
    # Shadow headers FIRST: PSP stand-ins for upstream headers whose real
    # version drags in desktop-only code (see src/platform/psp/shadow/*).
    ${WOWEE_ROOT}/src/platform/psp/shadow
    ${WOWEE_ROOT}/include
    ${WOWEE_ROOT}/src
    # <openssl/*.h> resolves to the mbedTLS shim, never to a host OpenSSL.
    ${WOWEE_ROOT}/src/platform/psp/openssl_compat
)

target_compile_definitions(wowee_psp PRIVATE
    WOWEE_PLATFORM_PSP=1
    WOWEE_PSP_BUILD=${WOWEE_PSP_BUILD}
    WOWEE_PSP_RELEASE="${WOWEE_PSP_RELEASE}"
    GLM_FORCE_PURE           # no SIMD intrinsics on Allegrex
)
if(WOWEE_PSP_SPLASH)
    target_compile_definitions(wowee_psp PRIVATE WOWEE_PSP_SPLASH=1)
endif()

# -G0: no small-data section (large programs overflow $gp range on PSP).
target_compile_options(wowee_psp PRIVATE
    -G0
    -Wall
    -Wextra
    -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-psabi>
    # Upstream socket code (net_platform.hpp, reached through public headers)
    # expects getaddrinfo()/select() from POSIX headers pspdev's newlib lacks;
    # a force-included compat header supplies them, upstream files untouched.
    $<$<COMPILE_LANGUAGE:CXX>:-include${WOWEE_ROOT}/src/platform/psp/posix_compat/psp_net_compat.h>
)

target_link_options(wowee_psp PRIVATE
    -Wl,--wrap=connect
    # PSP (docs/PSP_PORT_ARCHITECTURE.md): the big-block heap region (psp_bigheap.cpp). Every reference to
    # these - ours, libstdc++'s operator new/delete, zlib, mbedTLS, Lua - goes to __wrap_*; newlib's _malloc_r family
    # stays unwrapped (its internals keep their memory in newlib, freed there by address).
    -Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=memalign,--wrap=aligned_alloc
    -Wl,--wrap=malloc_usable_size
    -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/wowee_psp.map
)

# Every KB of code is a KB of heap less on the PSP-3000. --gc-sections
# drops the sections nothing references (libstdc++'s locales/streams/filesystem, dead upstream code), but a plain
# --gc-sections also drops the PSP's own tables - the import stubs, NIDs, module info - which nothing references by
# relocation. The toolchain's linker script, copied with KEEP() on those sections, keeps them.
option(WOWEE_PSP_GC_SECTIONS "Link with --gc-sections (the PSP sections kept)" ON)
if(WOWEE_PSP_GC_SECTIONS)
    set(_wowee_ld_in "$ENV{PSPDEV}/psp/lib/ldscripts/elf_mipsallegrexel_psp.x")
    set(_wowee_ld_out "${CMAKE_CURRENT_BINARY_DIR}/wowee_psp_gc.x")
    file(READ "${_wowee_ld_in}" _wowee_ld)
    set(_wowee_keep_pairs
        "{ *(.sceStub.text) *(SORT(.sceStub.text.*)) }|{ KEEP(*(.sceStub.text)) KEEP(*(SORT(.sceStub.text.*))) }"
        "{ *(.lib.ent.top) }|{ KEEP(*(.lib.ent.top)) }"
        "{ *(.lib.ent) }|{ KEEP(*(.lib.ent)) }"
        "{ *(.lib.ent.btm) }|{ KEEP(*(.lib.ent.btm)) }"
        "{ *(.lib.stub.top) }|{ KEEP(*(.lib.stub.top)) }"
        "{ *(.lib.stub) }|{ KEEP(*(.lib.stub)) }"
        "{ *(.lib.stub.btm) }|{ KEEP(*(.lib.stub.btm)) }"
        "{ *(.rodata.sceModuleInfo) }|{ KEEP(*(.rodata.sceModuleInfo)) }"
        "{ *(.rodata.sceResident) }|{ KEEP(*(.rodata.sceResident)) }"
        "{ *(.rodata.sceNid) }|{ KEEP(*(.rodata.sceNid)) }"
        "{ *(.rodata.sceVstub) *(SORT(.rodata.sceVstub.*)) }|{ KEEP(*(.rodata.sceVstub)) KEEP(*(SORT(.rodata.sceVstub.*))) }")
    foreach(_pair IN LISTS _wowee_keep_pairs)
        string(REPLACE "|" ";" _pair "${_pair}")
        list(GET _pair 0 _from)
        list(GET _pair 1 _to)
        string(FIND "${_wowee_ld}" "${_from}" _at)
        if(_at EQUAL -1)
            message(FATAL_ERROR "WOWEE_PSP_GC_SECTIONS: '${_from}' not in ${_wowee_ld_in} (toolchain changed?)")
        endif()
        string(REPLACE "${_from}" "${_to}" _wowee_ld "${_wowee_ld}")
    endforeach()
    file(WRITE "${_wowee_ld_out}" "${_wowee_ld}")
    target_link_options(wowee_psp PRIVATE -Wl,-T,${_wowee_ld_out} -Wl,--gc-sections)
endif()

# Order matters for psp-fixup-imports: a library that splits a module's stubs
# into two chunks leaves the second chunk dead on hardware ("stubs out of
# order"). tools/psp/build_psp.sh fails the build on that warning.
target_link_libraries(wowee_psp PRIVATE
    wowee_lua51  # FrameXML (plain code, no PSP module stubs)
    pspmp3
    pspmpeg  # sceMpeg stubs (firmware libmpeg, loaded with PSP_MODULE_AV_MPEGBASE for a movie)
    pspaudio
    mbedcrypto
    z
    pspgum
    pspgu
    pspge
    pspdisplay
    pspdebug
    pspctrl
    psppower
    psprtc
    pspopenpsid  # PSP (owner 14.10): the console's OpenPSID seals ms0:/WOWEE/account.dat
    pspkubridge
    # Wi-Fi. Deliberately NOT listed: psputility, pspnet_inet,
    # pspnet_resolver - pspdev's driver appends them after libc (libcglue's
    # sockets need them); naming them here too splits their stubs into two
    # chunks and the second chunk is dead on hardware.
    pspnet_apctl
    pspnet
    pspwlan
)

# Keep an unstripped copy for psp-addr2line / crash symbolication. Added before
# create_pbp_file so it runs before that macro's strip step.
add_custom_command(TARGET wowee_psp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:wowee_psp> ${CMAKE_CURRENT_BINARY_DIR}/wowee_psp.debug.elf
    COMMENT "Saving unstripped wowee_psp.debug.elf")
# The EBOOT gets the stripped ELF (create_pbp_file strips only for Release).
# With debug info it was 21 MB: slow to copy, and a copy cut short left a
# corrupt EBOOT on the card (black screen at launch, HOME dead).
add_custom_command(TARGET wowee_psp POST_BUILD
    COMMAND $ENV{PSPDEV}/bin/psp-strip $<TARGET_FILE:wowee_psp>
    COMMENT "Stripping wowee_psp for the EBOOT")

create_pbp_file(
    TARGET wowee_psp
    TITLE "WoWee PSP"
    VERSION ${WOWEE_PSP_RELEASE}
    MEMSIZE 1                  # full 64 MB on PSP-2000/3000 (MEMSIZE=1 in PARAM.SFO)
)
