#include "rendering/psp/psp_simple_spellfx.hpp"
#include "rendering/psp/psp_terrain.hpp"
#include "rendering/psp/psp_particles.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_prof.hpp"
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <cmath>
namespace wowee::rendering::psp {
namespace {
struct SpriteVertex { float u,v; uint32_t color; float x,y,z; };
alignas(16) uint32_t glow[32*32];
bool ready = false;
uint32_t color(SimpleSpell s) {
    if (s == SimpleSpell::Fire) return 0xFF2070FFu;
    if (s == SimpleSpell::Frost) return 0xFFFFD080u;
    if (s == SimpleSpell::Nature) return 0xFF50FF60u;
    if (s == SimpleSpell::Holy) return 0xFF80E8FFu;
    if (s == SimpleSpell::Physical) return 0xFFD0D0D0u;
    if (s == SimpleSpell::Arcane) return 0xFFFF80D0u;
    return 0xFFFF30A0u;
}
}
void drawSimpleSpellFx(const SimpleSpellFx& fx, const TerrainCamera& cam, int spriteBudget) {
    namespace pp = wowee::platform::psp;
    if (!fx.active() || spriteBudget <= 0) return;
    const uint32_t available = pp::displayListFree();
    if (available < kListReserveFx + 1024u) return;
    const int budget = std::min({spriteBudget, 32, static_cast<int>((available - kListReserveFx - 1024u) / (6 * sizeof(SpriteVertex) + 128))});
    if (!budget) return;
    PROF_SCOPE("spellfx.simple");
    if (!ready) {
        for (int y=0;y<32;++y) for(int x=0;x<32;++x) {
            const float dx=(x-15.5f)/15.5f, dy=(y-15.5f)/15.5f;
            const float a=std::max(0.0f,1.0f-dx*dx-dy*dy);
            glow[y*32+x]=(static_cast<uint32_t>(a*a*255)<<24)|0xFFFFFFu;
        }
        sceKernelDcacheWritebackRange(glow,sizeof(glow)); ready=true;
    }
    const glm::vec3 right=glm::normalize(glm::cross(cam.forward,cam.up)), up=glm::normalize(glm::cross(right,cam.forward));
    sceGuEnable(GU_DEPTH_TEST); sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_LIGHTING); sceGuDisable(GU_FOG); sceGuDisable(GU_CULL_FACE); sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND); sceGuBlendFunc(GU_ADD,GU_SRC_ALPHA,GU_FIX,0,0xFFFFFFFFu);
    sceGuEnable(GU_TEXTURE_2D); sceGuTexMode(GU_PSM_8888,0,0,GU_FALSE);
    sceGuTexImage(0,32,32,32,glow); sceGuTexFunc(GU_TFX_MODULATE,GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR,GU_LINEAR); sceGuTexLevelMode(GU_TEXTURE_CONST,0);
    sceGuTexWrap(GU_CLAMP,GU_CLAMP); sceGuTexScale(1,1); sceGuTexOffset(0,0);
    sceGuShadeModel(GU_SMOOTH); sceGumMatrixMode(GU_MODEL); sceGumLoadIdentity();
    int drawn=0;
    auto sprite=[&](const glm::vec3& at,float radius,uint32_t tint,float alpha) {
        if(drawn>=budget) return;
        const glm::vec3 p=at-cam.pos;
        const float depth=glm::dot(p,cam.forward);
        if(depth<cam.nearZ+radius || depth>std::min(cam.farZ,120.0f)) return;
        // Bound screen size and near-camera geometry for the PSP guard band.
        radius=std::min(radius,depth*.07f);
        const glm::vec3 r=right*radius,u=up*radius;
        tint=(tint&0xFFFFFFu)|(static_cast<uint32_t>(std::clamp(alpha,0.0f,1.0f)*255)<<24);
        auto* v=static_cast<SpriteVertex*>(sceGuGetMemory(6*sizeof(SpriteVertex)));
        const glm::vec3 corners[4]={p-r-u,p+r-u,p-r+u,p+r+u};
        const int order[6]={0,1,2,2,1,3};
        for(int i=0;i<6;++i) { const int j=order[i]; v[i]={float(j&1),float(j>>1),tint,corners[j].x,corners[j].y,corners[j].z}; }
        sceGumDrawArray(GU_TRIANGLES,GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D,6,nullptr,v); ++drawn;
    };
    for(const auto& e:fx.effects) {
        if(e.phase==SimpleSpellFx::Empty) continue;
        const uint32_t tint=color(e.spell);
        if(e.phase==SimpleSpellFx::Missile) {
            sprite(e.pos,.28f,tint,1);
            for(int i=1;i<7;++i) sprite(e.pos-e.direction*(i*.22f),.26f-i*.025f,tint,(7-i)/9.0f);
        } else if(e.phase==SimpleSpellFx::Impact) {
            const float t=e.elapsed/e.duration;
            sprite(e.pos,.3f+t*.9f,tint,1-t);
            for(int i=0;i<6;++i) { float a=i*1.04719755f; sprite(e.pos+glm::vec3(std::cos(a)*t*.8f,std::sin(a)*t*.8f,e.heal ? t*1.8f : t*.4f),.16f,tint,1-t); }
        } else if(e.phase==SimpleSpellFx::Area) {
            for(int i=0;i<8;++i) {
                const float a=e.elapsed+i*.78539816f;
                const float radius=1.8f+.2f*std::sin(e.elapsed*3);
                sprite(e.pos+glm::vec3(std::cos(a)*radius,std::sin(a)*radius,.25f+.2f*std::sin(a*2)),.25f,tint,.7f);
            }
        } else if(e.phase==SimpleSpellFx::Beam) {
            for(int i=0;i<8;++i) {
                const float t=std::fmod(e.elapsed*1.5f+i/8.0f,1.0f);
                sprite(e.pos+(e.end-e.pos)*t,.18f,tint,.85f);
            }
        } else {
            for(int i=0;i<8;++i) {
                const float a=e.elapsed*(e.phase==SimpleSpellFx::Cast ? 5.0f : 3.0f)+i*.78539816f;
                const float pulse=.75f+.25f*std::sin(e.elapsed*6.0f);
                sprite(e.pos+glm::vec3(std::cos(a)*.6f,std::sin(a)*.6f,(i%3-1)*.35f),.2f* pulse,tint,.65f);
            }
        }
    }
    sceGuDepthMask(GU_FALSE); sceGuDisable(GU_BLEND); sceGuDisable(GU_TEXTURE_2D);
    pp::textureResetBinding();
}
} // namespace wowee::rendering::psp
