# PSP asset converter (runs on the PC, MSYS2 UCRT64 or Linux).
#
# Reuses upstream's ADT/BLP parsers and TerrainMeshGenerator unchanged and
# bakes terrain into the "WTER" format read by src/rendering/psp/psp_terrain.
# A separate CMake project for the same reason as psp/: the top-level
# CMakeLists.txt stays identical to upstream.
#
#   cmake -S tools/psp/converter -B ../build-psp-conv -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build ../build-psp-conv
cmake_minimum_required(VERSION 3.20)
project(wowee_psp_converter CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(WOWEE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

find_package(glm CONFIG QUIET)
find_package(meshoptimizer CONFIG REQUIRED)  # MSYS2: mingw-w64-ucrt-x86_64-meshoptimizer

add_executable(psp_terrain_conv
    psp_terrain_conv.cpp
    ${WOWEE_ROOT}/src/pipeline/wdt_loader.cpp  # global-WMO maps (dungeons)
    ${WOWEE_ROOT}/src/pipeline/adt_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/adt_alpha.cpp
    ${WOWEE_ROOT}/src/pipeline/terrain_mesh.cpp
    ${WOWEE_ROOT}/src/pipeline/blp_loader.cpp
    ${WOWEE_ROOT}/src/core/logger.cpp
)
target_include_directories(psp_terrain_conv PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src)
if(TARGET glm::glm)
    target_link_libraries(psp_terrain_conv PRIVATE glm::glm)
endif()

# Doodads: upstream M2Loader/ADTLoader/BLPLoader + model classifier.
add_executable(psp_model_conv
    psp_model_conv.cpp
    particle_bake_l3.cpp  # the WSFX level-3 section (clean-room builder)
    ${WOWEE_ROOT}/src/pipeline/wdt_loader.cpp  # global-WMO maps (dungeons)
    ${WOWEE_ROOT}/src/pipeline/m2_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/wmo_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/adt_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/adt_alpha.cpp
    ${WOWEE_ROOT}/src/pipeline/blp_loader.cpp
    ${WOWEE_ROOT}/src/rendering/m2_model_classifier.cpp
    ${WOWEE_ROOT}/src/pipeline/dbc_loader.cpp  # GameObjectDisplayInfo (--gameobjects)
    ${WOWEE_ROOT}/src/core/logger.cpp
)
target_link_libraries(psp_model_conv PRIVATE meshoptimizer::meshoptimizer)
target_include_directories(psp_model_conv PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src ${WOWEE_ROOT}/extern)
if(TARGET glm::glm)
    target_link_libraries(psp_model_conv PRIVATE glm::glm)
endif()

# Characters: + DBC loader (nlohmann/json from extern/), CharSections resolver.
add_executable(psp_char_conv
    psp_char_conv.cpp
    ${WOWEE_ROOT}/src/pipeline/m2_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/blp_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/dbc_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/dbc_layout.cpp
    ${WOWEE_ROOT}/src/pipeline/char_sections.cpp
    ${WOWEE_ROOT}/src/core/logger.cpp
)
target_link_libraries(psp_char_conv PRIVATE meshoptimizer::meshoptimizer)
target_include_directories(psp_char_conv PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src ${WOWEE_ROOT}/extern)
if(TARGET glm::glm)
    target_link_libraries(psp_char_conv PRIVATE glm::glm)
endif()

# Equipment (M10c): ItemDisplayInfo + HelmetGeosetVisData + CharStartOutfit.
add_executable(psp_item_conv
    psp_item_conv.cpp
    particle_bake_l3.cpp  # the WSFX level-3 section (weapon_fx_bake.hpp -> particle_bake.hpp)
    item_conv_rules.cpp  # clean room: item_bake.md B1-B9
    ${WOWEE_ROOT}/src/pipeline/m2_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/blp_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/dbc_loader.cpp
    ${WOWEE_ROOT}/src/core/logger.cpp
)
target_link_libraries(psp_item_conv PRIVATE meshoptimizer::meshoptimizer)  # model_bake.hpp (cape TextureBank)
target_include_directories(psp_item_conv PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src ${WOWEE_ROOT}/extern)
if(TARGET glm::glm)
    target_link_libraries(psp_item_conv PRIVATE glm::glm)
endif()

# Creatures and NPCs (M10b): + CreatureDisplayInfo tables and the model
# classifier's helper-creature rule.
add_executable(psp_creature_conv
    psp_creature_conv.cpp
    npc_outfit.cpp              # clean room: outfit_geosets.md 3.3
    gameobject_anim_select.cpp  # clean room: gameobject_anim_select.md
    ${WOWEE_ROOT}/src/pipeline/m2_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/blp_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/dbc_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/dbc_layout.cpp
    ${WOWEE_ROOT}/src/pipeline/char_sections.cpp
    ${WOWEE_ROOT}/src/rendering/m2_model_classifier.cpp
    ${WOWEE_ROOT}/src/core/logger.cpp
)
target_link_libraries(psp_creature_conv PRIVATE meshoptimizer::meshoptimizer)
target_include_directories(psp_creature_conv PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src ${WOWEE_ROOT}/extern)
if(TARGET glm::glm)
    target_link_libraries(psp_creature_conv PRIVATE glm::glm)
endif()

# Read-only MPQ extraction of selected folders (M10c): works while the client
# runs and holds the archives (upstream asset_extract opens them read/write).
# Optional: only when StormLib is installed (MSYS2: mingw-w64-ucrt-x86_64-stormlib).
find_library(PSP_STORMLIB NAMES StormLib stormlib storm)
find_path(PSP_STORMLIB_INCLUDE StormLib.h PATH_SUFFIXES StormLib)
if(PSP_STORMLIB AND PSP_STORMLIB_INCLUDE)
    add_executable(psp_mpq_extract psp_mpq_extract.cpp)
    target_include_directories(psp_mpq_extract PRIVATE ${PSP_STORMLIB_INCLUDE})
    target_link_libraries(psp_mpq_extract PRIVATE ${PSP_STORMLIB})
    if(WIN32)
        foreach(lib z bz2 tomcrypt tommath wininet)
            find_library(PSP_STORM_DEP_${lib} ${lib})
            if(PSP_STORM_DEP_${lib})
                target_link_libraries(psp_mpq_extract PRIVATE ${PSP_STORM_DEP_${lib}})
            endif()
        endforeach()
    else()
        find_package(ZLIB QUIET)
        find_library(PSP_STORM_BZ2 NAMES bz2 bzip2)
        if(ZLIB_FOUND)
            target_link_libraries(psp_mpq_extract PRIVATE ZLIB::ZLIB)
        endif()
        if(PSP_STORM_BZ2)
            target_link_libraries(psp_mpq_extract PRIVATE ${PSP_STORM_BZ2})
        endif()
    endif()
endif()

# PC check of the PSP equipment path (M10c): psp_equipment.cpp over a PC Pak,
# starter outfit -> geosets + composited body texture (PPM).
add_executable(psp_equip_preview
    psp_equip_preview.cpp
    ${WOWEE_ROOT}/src/rendering/psp/psp_equipment.cpp
    ${WOWEE_ROOT}/src/rendering/psp/psp_equipment_dress.cpp
    ${WOWEE_ROOT}/src/rendering/psp/psp_weapon_placement.cpp
)
target_include_directories(psp_equip_preview PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src)
if(TARGET glm::glm)
    target_link_libraries(psp_equip_preview PRIVATE glm::glm)
endif()

# The character-select backgrounds (Interface\Glues\Models\UI_<Race>)
# rendered into pictures for glue.pak.
add_executable(psp_glue_scene_conv
    psp_glue_scene_conv.cpp
    ${WOWEE_ROOT}/src/pipeline/m2_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/blp_loader.cpp
    ${WOWEE_ROOT}/src/core/logger.cpp
)
target_include_directories(psp_glue_scene_conv PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src ${WOWEE_ROOT}/extern)
if(TARGET glm::glm)
    target_link_libraries(psp_glue_scene_conv PRIVATE glm::glm)
endif()

# Zone music (M12): upstream game::ZoneManager unchanged + ADT area ids.
add_executable(psp_audio_conv
    psp_audio_conv.cpp
    ${WOWEE_ROOT}/src/game/zone_manager.cpp
    ${WOWEE_ROOT}/src/pipeline/adt_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/adt_alpha.cpp
    ${WOWEE_ROOT}/src/pipeline/dbc_loader.cpp
    ${WOWEE_ROOT}/src/core/logger.cpp
)
target_include_directories(psp_audio_conv PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src ${WOWEE_ROOT}/extern)
if(TARGET glm::glm)
    target_link_libraries(psp_audio_conv PRIVATE glm::glm)
endif()

# Factions - FactionTemplate.dbc + Faction.dbc -> data/dbc.pak
# (include/rendering/psp/psp_dbc_format.hpp; --selftest runs without client data).
add_executable(psp_dbc_conv
    psp_dbc_conv.cpp
    ${WOWEE_ROOT}/src/pipeline/dbc_loader.cpp
    ${WOWEE_ROOT}/src/core/logger.cpp
)
target_include_directories(psp_dbc_conv PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src ${WOWEE_ROOT}/extern)
# Lighting - Light.dbc + LightParams + Light{Int,Float}Band -> data/light.pak
# (psp_light_format.hpp). Links the PSP's own lighting logic (--verify/--dump)
# and upstream's LightingManager unchanged (--verify --extracted compares them).
add_executable(psp_light_conv
    psp_light_conv.cpp
    ${WOWEE_ROOT}/src/rendering/psp/psp_lighting.cpp
    ${WOWEE_ROOT}/src/rendering/lighting_manager.cpp
    ${WOWEE_ROOT}/src/rendering/zone_ambience.cpp
    ${WOWEE_ROOT}/src/pipeline/dbc_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/dbc_layout.cpp
    ${WOWEE_ROOT}/src/pipeline/blp_loader.cpp
    ${WOWEE_ROOT}/src/core/logger.cpp
)
target_link_libraries(psp_light_conv PRIVATE meshoptimizer::meshoptimizer)  # forest_bake.hpp (PakView, writePng)
target_include_directories(psp_light_conv PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src ${WOWEE_ROOT}/extern)
if(TARGET glm::glm)
    target_link_libraries(psp_light_conv PRIVATE glm::glm)
endif()
# Sounds - effects (WAV -> s16 PCM by miniaudio, as upstream plays
# them), SoundEntries rows, footstep surface grids from the ADTs.
add_executable(psp_sound_conv
    psp_sound_conv.cpp
    ${WOWEE_ROOT}/src/pipeline/adt_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/adt_alpha.cpp
    ${WOWEE_ROOT}/src/pipeline/dbc_loader.cpp
    ${WOWEE_ROOT}/src/core/logger.cpp
)
target_include_directories(psp_sound_conv PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src ${WOWEE_ROOT}/extern)
if(TARGET glm::glm)
    target_link_libraries(psp_sound_conv PRIVATE glm::glm)
endif()
if(NOT WIN32)
    target_link_libraries(psp_sound_conv PRIVATE m)
endif()
# FrameXML -> data/framexml.pak (include/ui/psp/fx_format.hpp).
# Upstream xml_parser / toc_parser / BLPLoader unchanged; Lua 5.1.5 from
# extern/ built with the PSP luaconf.h copy (float lua_Number, like the EBOOT),
# so constants fold the same way; chunks are written with a 32-bit size_t
# (fx_luadump32.c). The copy dance: a luaconf.h next to the .c files always
# wins over one on the include path (docs/PSP_LUA_NOTES.md).
enable_language(C)
set(FX_LUA_DIR ${CMAKE_CURRENT_BINARY_DIR}/lua51)
# Compiler only: no standard libraries are opened.
set(FX_LUA_CORE lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring
    ltable ltm lundump lvm lzio lauxlib)
set(FX_LUA_SOURCES)
file(GLOB FX_LUA_HEADERS ${WOWEE_ROOT}/extern/lua-5.1.5/src/*.h)
foreach(h ${FX_LUA_HEADERS})
    get_filename_component(hn ${h} NAME)
    if(NOT hn STREQUAL "luaconf.h")
        configure_file(${h} ${FX_LUA_DIR}/${hn} COPYONLY)
    endif()
endforeach()
configure_file(${WOWEE_ROOT}/src/platform/psp/lua/luaconf.h ${FX_LUA_DIR}/luaconf.h COPYONLY)
foreach(f ${FX_LUA_CORE})
    configure_file(${WOWEE_ROOT}/extern/lua-5.1.5/src/${f}.c ${FX_LUA_DIR}/${f}.c COPYONLY)
    list(APPEND FX_LUA_SOURCES ${FX_LUA_DIR}/${f}.c)
endforeach()
add_library(fx_lua51 STATIC ${FX_LUA_SOURCES} fx_luadump32.c)
target_include_directories(fx_lua51 PUBLIC ${FX_LUA_DIR})
option(FX_LUA_DOUBLE "Bake chunks with a double lua_Number (only for a PSP build made with WOWEE_LUA_DOUBLE=1)" OFF)
if(FX_LUA_DOUBLE)
    target_compile_definitions(fx_lua51 PUBLIC WOWEE_LUA_DOUBLE=1)
endif()

add_executable(psp_framexml_conv
    psp_framexml_conv.cpp
    ${WOWEE_ROOT}/src/ui/xml_parser.cpp
    ${WOWEE_ROOT}/src/addons/toc_parser.cpp
    ${WOWEE_ROOT}/src/pipeline/blp_loader.cpp
    ${WOWEE_ROOT}/src/core/logger.cpp
)
target_include_directories(psp_framexml_conv PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src)
target_link_libraries(psp_framexml_conv PRIVATE fx_lua51)
if(TARGET glm::glm)
    target_link_libraries(psp_framexml_conv PRIVATE glm::glm)
endif()
# Map - minimap tiles (md5translate.trs), AreaTable names and the world map
# (WorldMapArea/Overlay + Interface\WorldMap) -> data/minimap_<map>.pak
# (include/rendering/psp/psp_minimap_format.hpp). Upstream's world map logic is linked
# unchanged for --verify / --selftest (the PSP links the same files); --selftest runs
# without client data.
add_executable(psp_minimap_conv
    psp_minimap_conv.cpp
    ${WOWEE_ROOT}/src/rendering/world_map/coordinate_projection.cpp
    ${WOWEE_ROOT}/src/rendering/world_map/exploration_state.cpp
    ${WOWEE_ROOT}/src/pipeline/dbc_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/blp_loader.cpp
    ${WOWEE_ROOT}/src/core/logger.cpp
)
target_link_libraries(psp_minimap_conv PRIVATE meshoptimizer::meshoptimizer)  # forest_bake.hpp (PakView, writePng)
target_include_directories(psp_minimap_conv PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src ${WOWEE_ROOT}/extern)
if(TARGET glm::glm)
    target_link_libraries(psp_minimap_conv PRIVATE glm::glm)
endif()
# Spell effects - SpellVisual / SpellVisualKit / SpellVisualEffectName
# + the effect M2s (upstream M2Loader) -> data/spellfx.pak (include/rendering/psp/psp_spellfx_format.hpp,
# tools/psp/converter/psp_spellfx_conv.cpp).
add_executable(psp_spellfx_conv
    psp_spellfx_conv.cpp
    particle_bake_l3.cpp  # the WSFX level-3 section (clean-room builder)
    ${WOWEE_ROOT}/src/pipeline/m2_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/blp_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/dbc_loader.cpp
    ${WOWEE_ROOT}/src/core/logger.cpp
)
target_link_libraries(psp_spellfx_conv PRIVATE meshoptimizer::meshoptimizer)
target_include_directories(psp_spellfx_conv PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src ${WOWEE_ROOT}/extern)
if(TARGET glm::glm)
    target_link_libraries(psp_spellfx_conv PRIVATE glm::glm)
endif()

# The equipment matrix: psp_equip_preview --matrix compares the PSP's
# equipment path with upstream's own equipment code run on the PC. The upstream files below compile
# unchanged with equip_matrix/seam first on the include path: a renderer that records what it is asked
# for (geosets, the body composite, textures, attachments), the GameHandler answers (the own player's
# inventory, the show-helm/cloak toggles) and an AssetManager over psp_mpq_extract folders.
# GameScreen::updateCharacterGeosets / updateCharacterTextures (the own player's rules) live in a UI
# file that cannot be compiled alone: their text is cut out of src/ui/game_screen_hud.cpp here, at
# configure time (a change there re-runs CMake), into equip_matrix_local.inc.
set(EQM_HUD ${WOWEE_ROOT}/src/ui/game_screen_hud.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${EQM_HUD})
file(READ ${EQM_HUD} EQM_HUD_TEXT)
string(REPLACE "\r\n" "\n" EQM_HUD_TEXT "${EQM_HUD_TEXT}")
set(EQM_LOCAL_INC "// Generated by tools/psp/converter/CMakeLists.txt (equip matrix) from src/ui/game_screen_hud.cpp.\n")
foreach(EQM_FN updateCharacterGeosets updateCharacterTextures)
    set(EQM_SIG "void GameScreen::${EQM_FN}(game::Inventory& inventory) {")
    string(FIND "${EQM_HUD_TEXT}" "${EQM_SIG}" EQM_AT)
    # A failed cut warns and leaves a stub (the matrix then reports the own-player rows wrong) - the
    # other converters still configure.
    set(EQM_STUB "void matrix_${EQM_FN}(MatrixApp&, core::AppearanceComposer*, game::Inventory&) {}  // cut failed\n")
    if(EQM_AT EQUAL -1)
        message(WARNING "equip-matrix: '${EQM_SIG}' is not in ${EQM_HUD} - update the cut in tools/psp/converter/CMakeLists.txt")
        string(APPEND EQM_LOCAL_INC "\n${EQM_STUB}")
        continue()
    endif()
    string(SUBSTRING "${EQM_HUD_TEXT}" ${EQM_AT} -1 EQM_REST)
    string(FIND "${EQM_REST}" "\n}\n" EQM_END)
    math(EXPR EQM_LEN "${EQM_END} + 3")
    string(SUBSTRING "${EQM_REST}" 0 ${EQM_LEN} EQM_BODY)
    string(REPLACE "${EQM_SIG}"
        "void matrix_${EQM_FN}(MatrixApp& matrixApp, [[maybe_unused]] core::AppearanceComposer* appearanceComposer_, game::Inventory& inventory) {"
        EQM_BODY "${EQM_BODY}")
    string(REPLACE "auto& app = core::Application::getInstance();" "auto& app = matrixApp;  // equip matrix"
        EQM_BODY "${EQM_BODY}")
    string(FIND "${EQM_BODY}" "auto& app = matrixApp;" EQM_OK)
    if(EQM_OK EQUAL -1)
        message(WARNING "equip-matrix: GameScreen::${EQM_FN} no longer starts from core::Application - update the cut")
        string(APPEND EQM_LOCAL_INC "\n${EQM_STUB}")
        continue()
    endif()
    string(APPEND EQM_LOCAL_INC "\n${EQM_BODY}")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/equip_matrix/equip_matrix_local.inc "${EQM_LOCAL_INC}")

add_library(equip_matrix_upstream OBJECT
    equip_matrix/seam_impl.cpp
    equip_matrix/upstream_ref.cpp
    equip_matrix/upstream_local.cpp
    ${WOWEE_ROOT}/src/core/entity_spawner.cpp
    ${WOWEE_ROOT}/src/core/entity_spawner_player.cpp
    ${WOWEE_ROOT}/src/core/appearance_composer.cpp
    ${WOWEE_ROOT}/src/core/helm_visual.cpp
    ${WOWEE_ROOT}/src/pipeline/item_textures.cpp
    ${WOWEE_ROOT}/src/pipeline/m2_asset_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/char_sections.cpp
    ${WOWEE_ROOT}/src/pipeline/m2_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/blp_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/dbc_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/dbc_layout.cpp
    ${WOWEE_ROOT}/src/core/logger.cpp
    ${WOWEE_ROOT}/src/game/inventory.cpp
    ${WOWEE_ROOT}/src/game/entity.cpp
    ${WOWEE_ROOT}/src/game/character.cpp
    ${WOWEE_ROOT}/src/game/update_field_table.cpp
    ${WOWEE_ROOT}/src/rendering/m2_model_classifier.cpp
    ${WOWEE_ROOT}/src/pipeline/wmo_loader.cpp
)
target_include_directories(equip_matrix_upstream BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/equip_matrix/seam ${CMAKE_CURRENT_BINARY_DIR}/equip_matrix)
target_include_directories(equip_matrix_upstream PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src ${WOWEE_ROOT}/extern)
if(TARGET glm::glm)
    target_link_libraries(equip_matrix_upstream PRIVATE glm::glm)
endif()
target_sources(psp_equip_preview PRIVATE equip_matrix/matrix_driver.cpp $<TARGET_OBJECTS:equip_matrix_upstream>)
target_compile_definitions(psp_equip_preview PRIVATE WOWEE_MATRIX_REPO="${WOWEE_ROOT}")
target_include_directories(psp_equip_preview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/equip_matrix)

# The M2 particle comparison tool on the PC (docs/PSP_PORT_ARCHITECTURE.md): a reference built from the
# level-3 specs (ptc_clean_reference.cpp) against the PSP engine's own code over the baked blob.
add_executable(psp_particle_matrix
    psp_particle_matrix.cpp
    particle_bake_l3.cpp
    ptc_clean_reference.cpp
    ${WOWEE_ROOT}/src/rendering/psp/psp_particle_sim.cpp  # the PSP engine's particle rules
    ${WOWEE_ROOT}/src/rendering/psp/psp_particles_l3.cpp  # the PSP engine (level-3 rules, portable)
    ${WOWEE_ROOT}/src/pipeline/m2_loader.cpp
    ${WOWEE_ROOT}/src/pipeline/blp_loader.cpp
    ${WOWEE_ROOT}/src/rendering/m2_model_classifier.cpp
    ${WOWEE_ROOT}/src/core/logger.cpp
)
target_link_libraries(psp_particle_matrix PRIVATE meshoptimizer::meshoptimizer)
target_include_directories(psp_particle_matrix PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src ${WOWEE_ROOT}/extern)
if(TARGET glm::glm)
    target_link_libraries(psp_particle_matrix PRIVATE glm::glm)
endif()

# The guild tabard emblems - Textures\GuildEmblems\*.blp -> data/emblems.pak (WLAY layers
# for the torso sections, include/rendering/psp/psp_emblem_format.hpp).
add_executable(psp_emblem_conv
    psp_emblem_conv.cpp
    ${WOWEE_ROOT}/src/pipeline/blp_loader.cpp
    ${WOWEE_ROOT}/src/core/logger.cpp
)
target_include_directories(psp_emblem_conv PRIVATE ${WOWEE_ROOT}/include ${WOWEE_ROOT}/src ${WOWEE_ROOT}/extern)
if(TARGET glm::glm)
    target_link_libraries(psp_emblem_conv PRIVATE glm::glm)
endif()
