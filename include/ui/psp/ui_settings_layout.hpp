#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>
namespace wowee::ui::psp {
struct SettingsLayoutRow {size_t row;float top;int section;bool heading;float height=0;};
template<class Rows,class Section>
inline float buildSettingsLayout(const Rows& rows,Section&& section,float rowHeight,float headingHeight,
                                 std::vector<SettingsLayoutRow>& out){
    out.clear();out.reserve(rows.size());float y=0;int previous=-1;
    for(size_t row:rows){const int group=section(row);const bool heading=group!=previous;
        if(heading)y+=headingHeight;out.push_back({row,y,group,heading,rowHeight});y+=rowHeight;previous=group;}
    return y;
}
inline float growSettingsRow(std::vector<SettingsLayoutRow>& rows,size_t id,float height){
    float added=0;
    for(auto& row:rows){row.top+=added;if(row.row==id){added=std::max(0.f,height-row.height);row.height+=added;}}
    return added;
}
inline float settingsScrollTo(float scroll,float top,float rowHeight,float viewport,float content){
    if(top<scroll)scroll=top;
    if(top+rowHeight>scroll+viewport)scroll=top+rowHeight-viewport;
    return std::clamp(scroll,0.f,std::max(0.f,content-viewport));
}
}
