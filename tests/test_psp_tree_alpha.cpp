#include "tools/psp/converter/texture_alpha_policy.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace wowee;
int main(int argc,char** argv) {
    pipeline::BLPImage bark;bark.width=bark.height=32;bark.data.resize(32*32*4);
    for(size_t i=0;i<bark.data.size();i+=4){bark.data[i]=120;bark.data[i+1]=90;bark.data[i+2]=65;bark.data[i+3]=(i%8)?255:0;}
    assert(bark.hasTransparency() && !bark.alphaIsSilhouette());
    auto opaque=bark;pspconv::prepareMaterialTextureAlpha(opaque,true,true);assert(!opaque.hasTransparency());
    auto blended=bark;pspconv::prepareMaterialTextureAlpha(blended,false,true);assert(blended.data==bark.data);
    auto leaves=bark;
    for(size_t i=0;i<leaves.data.size();i+=4)if(!leaves.data[i+3])leaves.data[i]=leaves.data[i+1]=leaves.data[i+2]=0;
    assert(leaves.alphaIsSilhouette());auto original=leaves.data;
    pspconv::prepareMaterialTextureAlpha(leaves,true,true);assert(leaves.data==original);
    pspconv::prepareMaterialTextureAlpha(leaves,true,false);assert(!leaves.hasTransparency());
    unsigned checked=0,repaired=0,silhouettes=0;
    if(argc>1)for(const auto& entry:std::filesystem::recursive_directory_iterator(argv[1])) {
        if(!entry.is_regular_file() || entry.path().extension()!=".blp")continue;
        auto name=entry.path().filename().string();
        if(name.find("trunk")==std::string::npos && name.find("bark")==std::string::npos && name.find("canopy")==std::string::npos && name.find("elwynntree")==std::string::npos)continue;
        std::ifstream f(entry.path(),std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});
        auto image=pipeline::BLPLoader::load(bytes);if(image.data.empty())continue;
        const bool alpha=image.hasTransparency(),silhouette=image.alphaIsSilhouette();auto before=image.data;
        pspconv::prepareMaterialTextureAlpha(image,true,true);
        if(silhouette){assert(image.data==before);++silhouettes;}
        else {assert(!image.hasTransparency());if(alpha){++repaired;std::printf("opaque alpha repaired: %s\n",entry.path().string().c_str());}}
        ++checked;
    }
    std::printf("Tree alpha passed: %u client textures checked, %u opaque atlas masks repaired, %u silhouettes preserved.\n",checked,repaired,silhouettes);
}
