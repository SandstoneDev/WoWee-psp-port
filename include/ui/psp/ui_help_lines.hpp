#pragma once
#include <string>
#include <vector>
namespace wowee::ui::psp {
template<class Measure>
std::vector<std::string> helpLines(const std::string& text,float maxWidth,Measure measure) {
    std::vector<std::string> out;
    std::string line,word;
    auto emit=[&](){out.push_back(line);line.clear();};
    auto append=[&](){
        if(word.empty())return;
        if(!line.empty() && measure(line+" "+word)>maxWidth)emit();
        if(!line.empty())line+=' ';
        // Split oversized words only at UTF-8 codepoint boundaries.
        for(size_t i=0;i<word.size();) {
            size_t end=i+1;
            while(end<word.size() && (static_cast<unsigned char>(word[end])&0xC0)==0x80)++end;
            const std::string cp=word.substr(i,end-i);
            if(!line.empty() && measure(line+cp)>maxWidth)emit();
            line+=cp;i=end;
        }
        word.clear();
    };
    for(char c:text) {
        if(c==' ' || c=='\n' || c=='\t'){append();if(c=='\n')emit();}
        else word+=c;
    }
    append();if(!line.empty())emit();
    return out;
}
}
