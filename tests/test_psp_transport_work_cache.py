"""Compare cached transport placement transforms with full per-frame rebuilding."""
from pathlib import Path
import re,subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/ui/psp/ui_world_transports.cpp').read_text()
a=source.index('void placeParts(');b=source.index('\n// The object',a)
method=source[a:b]
gate=re.search(r'if\(swap \|\| slotMoved\[si\].*?\)place\(\);',source).group()
cpp=r'''
#include "rendering/psp/psp_model_format.hpp"
#include "rendering/psp/psp_transport_format.hpp"
#include "rendering/psp/psp_transport_rider.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <array>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cassert>
#include <cstdio>
namespace rp=wowee::rendering::psp;
struct TrUi {
 struct Wmo {std::vector<rp::TransportPart> parts;};
 struct Obj {uint64_t guid=0;float yaw=0,scale=1;glm::vec3 pos{};Wmo* wmo=nullptr;
  struct {uint16_t model=0;int centerCm[3]={};int radiusCm=100;} m2;};
};
'''
main=r'''
int main(){
 TrUi::Wmo wmo;rp::TransportPart part{};part.model=1;part.axes[0]=part.axes[4]=part.axes[8]=1;
 part.position[2]=2;part.sphere[3]=3;wmo.parts.push_back(part);
 TrUi::Obj objs[3];for(int i=0;i<3;++i){objs[i].guid=i+1;objs[i].wmo=i<2?&wmo:nullptr;}
 std::vector<rp::DoodadPlacement> cached[3],full;
 std::vector<uint64_t> sets[3];unsigned builds[3]={};
 for(int frame=0;frame<1000;++frame){
  bool objectDataChanged=frame==200||frame==400;
  if(frame==200)objs[1].scale=1.7f;
  if(frame==400)objs[2].yaw=1.2f;
  objs[0].pos.z=std::sin(frame*.01f)*10;objs[0].yaw=frame*.001f; // live elevator/ship
  std::array<bool,3> slotMoved={true,false,false};
  for(int si=0;si<3;++si){
   auto& set=sets[si];set={objs[si].guid};
   struct {std::vector<uint64_t> set;} s{set};
   const bool swap=frame==0||frame==500; // first load and explicit reload
   auto place=[&](){cached[si].clear();placeParts(objs[si],cached[si]);++builds[si];};
   GATE
   full.clear();placeParts(objs[si],full);
   assert(cached[si].size()==full.size());
   assert(std::memcmp(cached[si].data(),full.data(),full.size()*sizeof(full[0]))==0);
  }
 }
 assert(builds[0]==1000&&builds[1]==4&&builds[2]==4);
 std::puts("Transport work passed: 3000 reference frames, live moving transforms, scale/rotation changes and reloads; static builds 1000 -> 4 per slot.");
}
'''.replace('GATE',gate)
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-transports-') as temp:
 folder=Path(temp);src=folder/'transport.cpp';exe=folder/'transport.exe';src.write_text(cpp+method+main)
 includes=[root/'include',root.parent/'tooling/glm-1.0.1']
 if Path(compiler).stem.lower()=='cl':
  args=[compiler,'/nologo','/std:c++20','/EHsc','/O2']+['/I'+str(p) for p in includes]+[str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'transport.obj')]
 else:args=[compiler,'-std=c++20','-O2']+['-I'+str(p) for p in includes]+[str(src),'-o',str(exe)]
 subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
