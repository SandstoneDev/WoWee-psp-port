#include "game/world_packets.hpp"
#include "network/packet.hpp"
#include "platform/psp/psp_spellfx_events.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
using namespace wowee;
network::Packet makePacket(bool dest,bool transport,bool truncate=false){
    network::Packet p(0);
    p.writePackedGuid(1);p.writePackedGuid(1);p.writeUInt8(0);
    p.writeUInt32(5740);p.writeUInt32(0);p.writeUInt32(0);p.writeUInt8(0);p.writeUInt8(0);
    p.writeUInt32(dest?0x40:0);
    if(dest){p.writePackedGuid(transport?99:0);p.writeFloat(10);p.writeFloat(20);if(!truncate)p.writeFloat(3);}
    return p;
}
struct Sink:platform::psp::SpellFxSink{platform::psp::SpellFxEvent last;void push(const platform::psp::SpellFxEvent&e)override{last=e;}};
static void checkDestination(){
    game::SpellGoData d;
    auto p=makePacket(true,false);assert(game::SpellGoParser::parse(p,d));assert(d.hasDestination&&d.destination[0]==10&&d.destination[1]==20&&d.destination[2]==3&&!d.destinationTransport);
    p=makePacket(false,false);assert(game::SpellGoParser::parse(p,d)&&!d.hasDestination);
    p=makePacket(true,true);assert(game::SpellGoParser::parse(p,d)&&d.destinationTransport==99);
    p=makePacket(true,false,true);assert(game::SpellGoParser::parse(p,d)&&!d.hasDestination);
    Sink sink;platform::psp::spellFxSink()=&sink;
    const float where[3]={10,20,3};const std::vector<uint64_t> targets;
    platform::psp::spellFxGo(1,5740,targets,where);assert(sink.last.hasDestination&&sink.last.destination[1]==20);
    platform::psp::spellFxSink()=nullptr;
    std::puts("ground-target packet parsing, reset, transport and truncation passed");
}

#ifdef WOWEE_PSP_STANDALONE_TEST
int main(){checkDestination();}
#else
#include <catch_amalgamated.hpp>
TEST_CASE("PSP spell effects preserve ground destinations") { checkDestination(); }
#endif
