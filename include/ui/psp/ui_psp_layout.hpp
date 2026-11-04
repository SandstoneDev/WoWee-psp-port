#pragma once
#include "ui/psp/ui_renderer.hpp"
namespace wowee::ui::psp::psp_layout {
// All drawing and focus rectangles share these 480x272 coordinates.
inline constexpr float barScale=20.f/18.f;
inline Rect action(int i) {
    return {36.f+(8.f+42.f*i)*(.5f*barScale),272.f-40.f*(.5f*barScale),20,20};
}
inline Rect xp(){return {36,272.f-27.f*barScale,256.f*barScale,7.f*barScale};}
inline Rect hints(){return {360,205,116,65};}
inline Rect menu(){return {26,28,428,220};}
inline Rect menuButton(int i){return {40.f+136.f*(i%3),52.f+31.f*(i/3),128,26};}
inline Rect menuBack(){return {126,211,228,24};}
inline Rect inventoryPanel(){return {4,4,472,264};}
inline Rect inventoryCell(int i){return {14.f+35.f*(i%10),50.f+35.f*(i/10),32,32};}
inline Rect inventoryHints(){return {368,42,100,184};}
inline constexpr float castY=156,watchBottom=200;
}
