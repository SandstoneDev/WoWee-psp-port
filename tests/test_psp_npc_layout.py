"""Exercise the production dialog builder/draw path with host fonts and UI capture."""
from pathlib import Path
import subprocess, sys, tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/ui/psp/ui_world_npc_frame.cpp').read_text()
def function(text,signature):
    start=text.index(signature);body=text.index('{',start);end=body+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]+'\n'
packets=(ROOT/'include/game/world_packets.hpp').read_text()
types='\n'.join(function(packets,'struct '+n)+';' for n in [
    'GossipOption','GossipQuestItem','GossipMessageData','QuestRewardItem',
    'QuestDetailsData','QuestRequestItemsData','QuestOfferRewardData'])
helpers=source[source.index('// Ids shared'):source.index('// The ruRU client')]
functions=''.join(function(source,s) for s in ['void WorldScreen::drawPanelButton(', 'bool WorldScreen::drawNpcFrame('])
cpp=r'''
#include "ui/psp/ui_npc_layout_cache.hpp"
#include "game/text_tokens.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
namespace game=wowee::game;
using wowee::ui::psp::NpcLayoutKey;using wowee::ui::psp::NpcFrameLayout;
namespace wowee::game {
const char* getClassName(Class){return "Warrior";}
const char* getRaceName(Race){return "Human";}
TYPES
}
#define PROF_SCOPE(...) ((void)0)
#define PSP_LOGI(...) ((void)0)
#define CLIENT_STR(x) #x
unsigned measurements=0;
struct Font {
 int size=6;int measure(const char* s)const{++measurements;return int(std::strlen(s))*size;}
 int lineHeight()const{return 10;}
};
struct Rect {float x,y,w,h;};struct SrcRect {float x,y,w,h;};
enum class Align {Left,Center,Right};enum class Blend {Alpha,Add};
namespace pp {
struct Texture {void* pixels=reinterpret_cast<void*>(1);float uw=64,uh=16;};
struct NpcItemInfo {std::string name;uint32_t quality=1;};
std::string language="enUS";const std::string& localeActive(){return language;}
}
enum {kFontSmall,kFontNormal};
struct Focus {
 uint32_t id=0;std::map<uint32_t,Rect> stops;
 bool addStop(uint32_t n,const Rect& r){stops[n]=r;return id==n;}
 bool isPressed(uint32_t)const{return false;}uint32_t focused()const{return id;}
};
struct Ui {
 struct Text {float x,y;std::string s;uint32_t col;};std::vector<Text> texts;
 struct Fill {Rect r;uint32_t col;};std::vector<Fill> fills;
 float scale()const{return 1;}float offsetX()const{return 0;}float offsetY()const{return 0;}
 template<class...T>void image(T&&...){}template<class...T>void imageCircle(T&&...){}
 template<class...T>void fillCircle(T&&...){}void pushClip(const Rect&){}void popClip(){}
 void fill(const Rect& r,uint32_t col){fills.push_back({r,col});}
 void text(const Font& f,float x,float y,const char* s,uint32_t col,Align a=Align::Left){
  if(a!=Align::Left)x-=f.measure(s)*(a==Align::Center?.5f:1.f);texts.push_back({x,y,s,col});
 }
 void textShadow(const Font& f,float x,float y,const char* s,uint32_t col,Align a=Align::Left){text(f,x,y,s,col,a);}
 void textMarquee(const Font& f,const Rect& r,const char* s,uint32_t col,float width=-1){
  if(width<0)width=f.measure(s);text(f,r.x,r.y,s,col);
 }
};using UiRenderer=Ui;
struct Kit {
 Font fonts[2]={{6},{7}};uint32_t revision=1;
 const Font& font(int i)const{return fonts[i];}uint32_t fontRevision()const{return revision;}
 template<class...T>void button(T&&...)const{}
};
struct Session {
 game::Character me;uint64_t guid=1;uint32_t revision=1,names=1;
 game::GossipMessageData gossip;game::QuestDetailsData details;
 game::QuestRequestItemsData progress;game::QuestOfferRewardData reward;
 std::string greeting,npcText;std::map<uint32_t,pp::NpcItemInfo> items;
 const game::Character* activeCharacter()const{return &me;}uint64_t playerGuid()const{return guid;}
 uint32_t npcDialogVersion()const{return revision;}uint32_t creatureInfoVersion()const{return names;}
 const auto& getCurrentGossip()const{return gossip;}const auto& getQuestDetails()const{return details;}
 const auto& getQuestRequestItems()const{return progress;}const auto& getQuestOfferReward()const{return reward;}
 const auto& getQuestGreeting()const{return greeting;}const std::string& getNpcText(uint32_t)const{return npcText;}
 const pp::NpcItemInfo* npcItemInfo(uint32_t id)const{auto p=items.find(id);return p==items.end()?nullptr:&p->second;}
};
struct RuText {std::string cls[11][2],race[12][2];};
RuText& ruText(){static RuText t;return t;}
std::string declineMarkup(const std::string& s){return s;}
struct WorldScreen {
 enum {kWinGossip=1,kWinQuestDetails,kWinQuestProgress,kWinQuestReward};
 enum {kNpcIconGossip=0,kNpcIconAvailable=11,kNpcIconActive,kNpcIconIncomplete,kNpcIconDaily,kNpcIconDailyActive,kNpcIconCount};
 struct UnitView {std::string name;};std::string npcName="Questgiver";
 Kit kit_;Session session_;Focus winFocus_;std::vector<uint8_t> windows_;
 struct {bool guard=false;int rewardChoice=-1,scroll=0;} npc_;
 struct {pp::Texture frame,titleHl,btnUp,btnDown,btnDis,btnHl,close,closeDown,scrUp,scrDown,scrUpDis,scrDownDis,money,icons[16];} npcArt_;
 struct {pp::Texture slot;} spellArt_;uint32_t spellFrame_=1;
 uint32_t tipProtoItem_=0,tipProtoCount_=0;Rect tipProtoRect_{};
 std::array<NpcFrameLayout,4> npcLayouts_;
 struct Icons {pp::Texture tex;const pp::Texture* byDisplay(uint32_t,uint32_t){return &tex;}} icons;
 Icons& itemIcons(){return icons;}bool unitView(uint64_t,UnitView& u){u.name=npcName;return true;}
 const pp::Texture* portraitOf(uint64_t)const{return nullptr;}
 void loadNpcArt(){}
 void drawPanelButton(UiRenderer&,uint32_t,const Rect&,const char*,bool,bool,float=-1.f);
 bool drawNpcFrame(UiRenderer&,uint8_t);
 bool drawNpcFrameReference(UiRenderer&,uint8_t);
};
'''.replace('TYPES',types)
main=r'''
void compare(const Ui& a,const Ui& b){
 assert(a.texts.size()==b.texts.size());
 for(size_t i=0;i<a.texts.size();++i){const auto& x=a.texts[i];const auto& y=b.texts[i];
  assert(x.s==y.s&&x.col==y.col&&std::fabs(x.x-y.x)<.001f&&std::fabs(x.y-y.y)<.001f);
 }
 assert(a.fills.size()==b.fills.size());
 for(size_t i=0;i<a.fills.size();++i)assert(a.fills[i].col==b.fills[i].col);
}
int main(){
 WorldScreen w;w.session_.me.name="Busch";w.session_.me.race=game::Race::HUMAN;
 w.session_.me.characterClass=game::Class::WARRIOR;w.session_.me.gender=game::Gender::MALE;
 auto& s=w.session_;s.gossip.npcGuid=s.details.npcGuid=s.progress.npcGuid=s.reward.npcGuid=42;
 s.gossip.titleTextId=1;s.greeting="Welcome $N, $C of the $R. $gsir:madam;";
 game::GossipQuestItem q;q.title="A quest for $N";q.questIcon=2;s.gossip.quests={q};
 game::GossipOption o;o.text="Show your goods, $N";s.gossip.options={o};
 s.details.title="A favor for $N";s.details.details="";
 for(int i=0;i<30;++i)s.details.details+="Help the village and return with supplies. ";
 s.details.objectives="Gather supplies, $N.";s.details.rewardMoney=12345;s.details.rewardXp=100;
 s.items[10]={"A long reward item name that wraps",2};s.items[11]={"Shield",1};
 s.details.rewardItems={{10,4,100}};s.details.rewardChoiceItems={{11,1,101}};
 s.progress.title="Deliver supplies";s.progress.completionText="Have you brought them, $N?";
 s.progress.requiredItems={{10,4,100}};s.progress.requiredMoney=125;
 s.reward.title="Thank you $N";s.reward.rewardText="Take your reward.";s.reward.rewardMoney=12345;
 s.reward.choiceRewards={{10,4,100},{11,1,101}};s.reward.fixedRewards={{11,1,101}};
 for(uint8_t kind=1;kind<=4;++kind){
  w.windows_={kind};w.npc_.scroll=0;w.winFocus_.id=0;Ui first;assert(w.drawNpcFrame(first,kind));
  auto& cache=w.npcLayouts_[kind-1];assert(cache.valid&&cache.builds==1&&!first.texts.empty());
  assert(cache.total>0);const unsigned baselineMeasures=measurements;
  REFERENCE
  const unsigned before=measurements;
  for(int n=0;n<1000;++n){Ui next;assert(w.drawNpcFrame(next,kind));compare(first,next);}
  assert(measurements==before&&cache.builds==1&&cache.hits>=1000);
  assert(baselineMeasures>0);
 }
 // Focus/scroll/reward highlighting change without reflowing.
 w.windows_={4};w.npc_.scroll=0;w.winFocus_.id=0x6131;w.npc_.rewardChoice=1;
 const auto before=measurements;Ui selected;w.drawNpcFrame(selected,4);
 assert(measurements==before&&w.npcLayouts_[3].builds==1);
 bool highlight=false;for(const auto& f:selected.fills)highlight|=f.col==0x6000D1FFu;assert(highlight);
 REFERENCE_SELECTED
 // Scrolling and overlapping windows reuse their own retained layouts.
 w.windows_={1,2};w.winFocus_.id=0;w.npc_.scroll=40;
 const auto scrollBefore=measurements;Ui scrolled;w.drawNpcFrame(scrolled,2);
 assert(measurements==scrollBefore&&w.npcLayouts_[1].builds==1&&w.npc_.scroll==40);
 Ui behind;w.drawNpcFrame(behind,1);assert(w.npcLayouts_[0].builds==1);
 // The same dialog identity can receive new text or late item metadata.
 s.greeting="New greeting, $N.";s.items[10].name="Late item name";++s.revision;
 w.windows_={1};w.winFocus_.id=0;w.npc_.scroll=0;Ui changed;w.drawNpcFrame(changed,1);
 assert(w.npcLayouts_[0].builds==2&&w.npcLayouts_[0].lines[0].s=="New greeting, Busch.");
 Ui late;w.windows_={4};w.drawNpcFrame(late,4);assert(w.npcLayouts_[3].builds==2);
 bool lateName=false;for(const auto& l:w.npcLayouts_[3].lines)lateName|=l.s=="Late item name";assert(lateName);
 // Character, locale/font reload and late NPC name changes invalidate independently.
 s.me.name="Xelera";s.me.gender=game::Gender::FEMALE;s.guid=2;w.windows_={1};Ui character;
 w.drawNpcFrame(character,1);assert(w.npcLayouts_[0].lines[0].s=="New greeting, Xelera.");
 const auto builds=w.npcLayouts_[0].builds;pp::language="ruRU";Ui locale;w.drawNpcFrame(locale,1);
 assert(w.npcLayouts_[0].builds==builds+1);
 ++w.kit_.revision;w.kit_.fonts[0].size=5;Ui fonts;w.drawNpcFrame(fonts,1);
 assert(w.npcLayouts_[0].builds==builds+2);
 w.npcName="Updated questgiver";++s.names;Ui name;w.drawNpcFrame(name,1);
 assert(w.npcLayouts_[0].name==w.npcName);
 // No greeting initially: a late text response must populate the retained lines.
 s.greeting.clear();s.npcText.clear();++s.revision;Ui empty;w.drawNpcFrame(empty,1);
 s.npcText="Late greeting for $N";++s.revision;Ui arrived;w.drawNpcFrame(arrived,1);
 assert(w.npcLayouts_[0].lines[0].s=="Late greeting for Xelera");
 std::puts("NPC layout passed: 4 dialogs x 1000 unchanged frames require zero measurements/rebuilds; live focus/rewards and late text/items/character/locale/font/name changes verified.");
}
'''
reference=''
if len(sys.argv)>2:
    reference=function(Path(sys.argv[2]).read_text(),'bool WorldScreen::drawNpcFrame(').replace('WorldScreen::drawNpcFrame(','WorldScreen::drawNpcFrameReference(',1)
    main=main.replace('REFERENCE_SELECTED','Ui oldSelected;w.drawNpcFrameReference(oldSelected,4);compare(selected,oldSelected);')
    main=main.replace('REFERENCE','Ui old;w.drawNpcFrameReference(old,kind);compare(first,old);')
else:main=main.replace('REFERENCE_SELECTED','').replace('REFERENCE','')
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-npc-layout-') as tmp:
    folder=Path(tmp);src=folder/'layout.cpp';exe=folder/'layout.exe'
    src.write_text(cpp+helpers+functions+reference+main)
    args=[compiler,'/nologo','/std:c++20','/EHsc','/O2','/I'+str(ROOT/'include'),str(src),str(ROOT/'src/game/text_tokens.cpp'),'/Fe:'+str(exe),'/Fo:'+str(folder)+'\\'] if Path(compiler).stem.lower()=='cl' else [compiler,'-std=c++20','-O2','-I'+str(ROOT/'include'),str(src),str(ROOT/'src/game/text_tokens.cpp'),'-o',str(exe)]
    subprocess.run(args,cwd=tmp,check=True);subprocess.run([str(exe)],check=True)
