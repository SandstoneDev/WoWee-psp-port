"""Run the production portrait camera and cache lifecycle on the host."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    body = source.index('{', start)
    end, depth = body + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


cpp = r'''
#include "rendering/psp/psp_portrait_camera.hpp"
#include <cassert>
#include <cstdint>
#include <limits>
namespace rendering::psp { struct TerrainCamera {}; }
#define PSP_LOGI(...) ((void)0)
struct Model {
    bool ready = true;
    uint32_t rev = 1;
    bool loaded() const { return ready; }
    uint32_t portraitRevision() const { return rev; }
};
struct Session { uint64_t guid = 10; uint64_t playerGuid() const { return guid; } };
struct WorldScreen {
    uint32_t ownPortraitVram_ = 8192, ownPortraitRevision_ = 0;
    uint64_t ownPortraitGuid_ = 0;
    bool ownPortraitOk_ = false, drawOk = true;
    int draws = 0;
    Model player_;
    Session session_;
    bool renderCharacterHead(Model&, uint32_t, const rendering::psp::TerrainCamera&) { ++draws; return drawOk; }
    void renderOwnPortrait(const rendering::psp::TerrainCamera&);
};
'''
cpp += method((ROOT / 'src/ui/psp/ui_world_target.cpp').read_text(), 'void WorldScreen::renderOwnPortrait(')
cpp += r'''
int main() {
    using namespace wowee::rendering::psp;
    // Short/tall and hunched characters: keep the face around its authored
    // attachment, with a tighter crop than the previous shoulders/full-crown view.
    for (float h : {.9f, 1.5f, 2.0f, 2.8f}) {
        for (float headZ : {.65f, .83f, .95f}) {
            auto c = characterPortraitCamera(h, glm::vec3(h * .08f, 0, h * headZ));
            float distance = glm::length(c.eye - c.centre);
            float half = distance * std::tan(glm::radians(c.fovy * .5f));
            assert(std::fabs(half-h*.15f)<.001f);
            assert(c.centre.z + half >= h*(headZ+.12f));
            assert(c.centre.z - half <= h*(headZ-.16f));
            const float oldHalf=std::max(h*.22f,h*1.035f-h*(headZ-.055f));
            assert(half/oldHalf<.70f); // at least 1.4x magnification for every sampled head
            assert(std::fabs(c.centre.x-h*.08f)<.001f);
            assert(c.eye.x > c.centre.x && c.eye.y < c.centre.y);
            assert(c.nearZ < distance && c.farZ > distance + h);
        }
    }
    auto invalid = characterPortraitCamera(std::numeric_limits<float>::quiet_NaN(), glm::vec3(0, 0, 100));
    assert(std::isfinite(invalid.eye.x));
    WorldScreen w;
    rendering::psp::TerrainCamera cam;
    w.renderOwnPortrait(cam);
    assert(w.ownPortraitOk_ && w.draws == 1);
    for (int i=0; i<600; ++i) w.renderOwnPortrait(cam);
    assert(w.draws == 1); // walking/combat never animates or redraws the portrait
    ++w.player_.rev;
    w.renderOwnPortrait(cam);
    assert(w.draws == 2 && w.ownPortraitRevision_ == 2);
    w.session_.guid = 20; w.drawOk = false;
    w.renderOwnPortrait(cam);
    assert(!w.ownPortraitOk_); // never show the old character after switching
    w.drawOk = true; w.renderOwnPortrait(cam);
    assert(w.ownPortraitOk_ && w.draws == 4);
    w.player_.ready = false; w.renderOwnPortrait(cam);
    assert(!w.ownPortraitOk_ && w.draws == 4);
    w.player_.ready = true; w.renderOwnPortrait(cam);
    assert(w.ownPortraitOk_ && w.draws == 5);
    w.ownPortraitVram_ = 0; ++w.player_.rev; w.renderOwnPortrait(cam);
    assert(w.draws == 5);
}
'''

if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = Path(tmp) / 'portrait.cpp', Path(tmp) / 'portrait.exe'
        src.write_text(cpp)
        compiler = os.environ.get('CXX', 'c++')
        subprocess.run([compiler, '-std=c++20', '-I' + str(ROOT / 'include'),
                        '-I' + str(ROOT.parent / 'tooling/glm-1.0.1'), str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('Player portrait camera and cache tests passed')
