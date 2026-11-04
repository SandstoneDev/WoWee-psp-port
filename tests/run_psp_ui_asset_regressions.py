"""Host regressions for PSP UI, portraits, loot and world asset policies.
Requires a GCC/Clang-compatible C++20 compiler; optional --sound-converter checks the real NPC voice baker.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--cxx",default=os.environ.get("CXX","c++"))
    parser.add_argument("--glm",type=Path,default=ROOT.parent/"tooling/glm-1.0.1")
    parser.add_argument("--sound-converter",type=Path)
    args=parser.parse_args()
    env=dict(os.environ,CXX=args.cxx)
    def run(command):
        subprocess.run([str(x) for x in command],cwd=ROOT,env=env,check=True)
    for name in ["forced_movement","decoration_removal","bag_navigation","movement_cast_audio","cast_sustain","stream_retry","fishing_owner","fishing_splash","death_audio","wmo_landmarks","window_menu","window_hints","object_fog","settings_categories","settings_dropdown","graphics_presets","player_portrait"]:
        run([sys.executable,ROOT/"tests"/("test_psp_"+name+".py"),args.cxx])
    cases={
        "action_assignment":[],
        "audio_math":[],
        "loot_requests":[],
        "loot_animation":["src/rendering/animation/activity_fsm.cpp"],
        "cast_pushback":["src/rendering/animation/combat_fsm.cpp","src/core/logger.cpp"],
        "bridge_memory":[],
        "placement_memory":[],
        "stream_readiness":[],
        "marker_visibility":[],
        "settings_layout":[],
        "frame_budget":[],
        "floor_support":[],
        "wmo_landmarks":[],
        "camera_ray":[],
        "wmo_batch":[],
        "rest_triggers":[],
        "fishing":["src/rendering/animation/combat_fsm.cpp","src/core/logger.cpp"],
        "tree_alpha":["src/pipeline/blp_loader.cpp","src/core/logger.cpp"],
    }
    with tempfile.TemporaryDirectory(prefix="wowee-ui-assets-") as temp:
        for name,dependencies in cases.items():
            exe=Path(temp)/(name+".exe")
            run([args.cxx,"-std=c++20","-O2","-I"+str(ROOT),"-I"+str(ROOT/"include"),
                 "-I"+str(ROOT/"extern"),"-I"+str(args.glm),ROOT/"tests"/("test_psp_"+name+".cpp"),
                 *[ROOT/p for p in dependencies],"-o",exe])
            run([exe])
    if args.sound_converter:
        run([sys.executable,ROOT/"tests/test_psp_npc_voice_bake.py",args.sound_converter])
    print("All PSP UI/asset regressions passed")

if __name__=="__main__":
    main()
