"""Exercise the production doodad iterator's landmark deduplication and governor path."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'include/rendering/psp/psp_bigwmo.hpp').read_text()
a=s.index('    template <typename Fn>\n    void forDoodads(');b=s.index('\n    // Collision in world space',a)
body=s[a:b]
code='''#include "rendering/psp/psp_bigwmo_format.hpp"
#include <algorithm>
#include <vector>
#include <cassert>
using namespace wowee::rendering::psp;
struct Fixture {
struct Res {void* blob;unsigned size;};
std::vector<Res> cellDoodads_;
std::vector<BigWmoDoodad> landmarks_;
unsigned doodadSet_=2;
'''+body+'''};
int main(){
Fixture f; BigWmoDoodad hero{};hero.index=343;hero.set=0;hero.model=674;
f.landmarks_.push_back(hero);
BigWmoDoodad regular{};regular.index=12;regular.set=2;
BigWmoDoodad otherSet{};otherSet.index=13;otherSet.set=1;
BigWmoDoodad cell[]={hero,regular,otherSet};f.cellDoodads_.push_back({cell,sizeof(cell)});
std::vector<unsigned> ids;
f.forDoodads([&](const auto& d){ids.push_back(d.index);});assert((ids==std::vector<unsigned>{343,12}));
ids.clear();f.forDoodads([&](const auto& d){ids.push_back(d.index);},true);assert((ids==std::vector<unsigned>{343}));
f.cellDoodads_.clear();ids.clear();f.forDoodads([&](const auto& d){ids.push_back(d.index);},true);assert((ids==std::vector<unsigned>{343}));
f.landmarks_[0].set=1;ids.clear();f.forDoodads([&](const auto& d){ids.push_back(d.index);});assert(ids.empty());
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.cpp').write_text(code);exe=p/'test.exe'
 subprocess.run([sys.argv[1],'-std=c++20','-I'+str(root/'include'),str(p/'test.cpp'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('Landmarks: governor-only rendering, duplicate filtering, optional cells absent and doodad sets passed.')
