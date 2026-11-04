"""Exercise production single-container closing and focus restoration."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/ui/psp/ui_world_bags.cpp').read_text(encoding='utf-8')
def function(signature):
    a=source.index(signature);b=source.index('{',a)+1;depth=1
    while depth:
        depth+=(source[b]=='{')-(source[b]=='}');b+=1
    return source[a:b]+'\n'
methods=''.join(function(s) for s in ['bool WorldScreen::bagSlotId(', 'void WorldScreen::closeBag(', 'bool WorldScreen::closeFocusedBag('])
cpp=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
namespace game {struct Inventory {static constexpr int FIRST_BAG_EQUIP_SLOT=19;};}
struct WorldScreen {
 static constexpr uint8_t kWinBags=0x51;
 static constexpr uint32_t kIdBagSlot0=0x6C00,kIdBagClose0=0x6CC0;
 bool bagOpen_[5]={},open=true,uiMode_=false;int invBag_=0;
 struct Focus {uint32_t id=0;int cancels=0;uint32_t focused(){return id;}void setFocus(uint32_t v){id=v;}void cancelPresses(){++cancels;}}winFocus_;
 void closeWindow(uint8_t){open=false;uiMode_=false;winFocus_.id=0;}void setUiMode(bool v){uiMode_=v;}
 static bool bagSlotId(uint32_t,int&,int&);void closeBag(int);bool closeFocusedBag();
};
'''
main=r'''
int main(){
 WorldScreen w;w.bagOpen_[0]=w.bagOpen_[1]=w.bagOpen_[3]=true;
 w.invBag_=0;w.winFocus_.id=w.kIdBagSlot0+36+4;
 assert(w.closeFocusedBag());assert(!w.bagOpen_[1]&&w.bagOpen_[0]&&w.bagOpen_[3]&&w.open);
 assert(w.invBag_==3&&w.winFocus_.id==w.kIdBagSlot0+3*36);
 w.closeFocusedBag();assert(!w.bagOpen_[3]&&w.bagOpen_[0]&&w.open&&w.invBag_==0);
 w.closeFocusedBag();assert(!w.open&&!w.uiMode_&&w.winFocus_.id==0);
 WorldScreen other;other.bagOpen_[4]=true;other.winFocus_.id=other.kIdBagClose0+4;
 other.closeFocusedBag();assert(!other.open&&!other.uiMode_&&other.winFocus_.id==0);
 other.closeBag(-1);other.closeBag(5);assert(!other.open);
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-bag-navigation-') as temp:
    src=Path(temp)/'bags.cpp';exe=Path(temp)/'bags.exe';src.write_text(cpp+methods+main,encoding='utf-8')
    subprocess.run([compiler,'-std=c++20',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Bag navigation passed: close selected container, preserve others, close the last bag without hidden bag-button focus')
