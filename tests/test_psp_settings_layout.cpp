#include "ui/psp/ui_settings_layout.hpp"
#include <cassert>
#include <cstdio>
int main(){using namespace wowee::ui::psp;std::vector<size_t> rows;for(size_t i=0;i<19;++i)rows.push_back(i);
 std::vector<SettingsLayoutRow> layout;
 const float total=buildSettingsLayout(rows,[](size_t i){return static_cast<int>(i/4);},18,15,layout);
 assert(layout.size()==19 && total>185);
 float scroll=0;
 for(const auto& row:layout){scroll=settingsScrollTo(scroll,row.top,18,185,total);assert(row.top>=scroll && row.top+18<=scroll+185);}
 assert(scroll>0);
 for(auto i=layout.rbegin();i!=layout.rend();++i){scroll=settingsScrollTo(scroll,i->top,18,185,total);assert(i->top>=scroll && i->top+18<=scroll+185);}
 assert(scroll<=15);
 const float oldTotal=total;
 const float extra=growSettingsRow(layout,0,52);
 assert(extra==34 && layout[0].height==52 && layout[1].top==rows[1]*18+15+extra);
 scroll=0;
 for(const auto& row:layout){scroll=settingsScrollTo(scroll,row.top,row.height,185,oldTotal+extra);assert(row.top>=scroll && row.top+row.height<=scroll+185);}
std::puts("Merged graphics layout: every setting reachable, focused rows stay inside the viewport, scrolling clamps both ends.");
}
