#include "platform/psp/psp_item_icons.hpp"
#include "platform/psp/psp_pak.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
uint32_t now = 100;
unsigned reads = 0;
bool failMapping = false, failIcon = false, available = true;
bool tight = false;
unsigned openFiles = 0;
}
namespace wowee::platform::psp {
uint32_t logUptimeMs() { return now; }
bool fsPath(char* out, std::size_t length, const char* relative) {
    std::snprintf(out,length,"%s",relative);
    return true;
}
bool fsExists(const char*) { return available; }
bool Pak::open(const char*) { return available; }
const PakEntry* Pak::find(const char* name) {
    static const PakEntry entries[] = {{"ihdr",16,24},{"dmap",64,258},{"icons",1024,6400}};
    for (const auto& entry : entries) if (!std::strcmp(name,entry.name)) return &entry;
    return nullptr;
}
void* Pak::load(const PakEntry*) {
    auto* data = static_cast<unsigned char*>(std::malloc(24));
    std::memcpy(data,"WIIC",4);
    const uint32_t fields[] = {1,100,64,128,4};
    std::memcpy(data+4,fields,sizeof(fields));
    return data;
}
void pakFree(void* data) { std::free(data); }
int pakOpenRetry(const char*) { return available ? 1 : -1; }
void pakNoteOpen(int,const char*) { ++openFiles; }
void pakNoteClose(int) { assert(openFiles); --openFiles; }
bool pakFdTight() { return tight; }
bool pakRead(int, uint32_t offset, void* data, uint32_t size) {
    ++reads;
    if (offset < 1024) {
        if (failMapping) { failMapping=false; return false; }
        const uint16_t slot = static_cast<uint16_t>(((offset-64)/2)%90+4);
        assert(size==2);
        std::memcpy(data,&slot,2);
    } else {
        if (failIcon) { failIcon=false; return false; }
        assert(size==64);
        std::memset(data,0,size);
        static_cast<unsigned char*>(data)[4]=static_cast<unsigned char>((offset-1024)/64);
    }
    return true;
}
bool textureFromBlob(void* block, uint32_t, Texture& texture, bool) {
    texture.block=block;
    texture.pixels=static_cast<unsigned char*>(block)+4;
    return true;
}
void textureRelease(Texture& texture) { std::free(texture.block); texture=Texture{}; }
}
int sceIoClose(int) { return 0; }
void sceKernelDcacheWritebackRange(void*,std::size_t) {}

unsigned slot(const wowee::platform::psp::Texture* texture) {
    assert(texture);
    return *static_cast<const unsigned char*>(texture->pixels);
}

int main() {
    using namespace wowee::platform::psp;
    ItemIconDb db;
    available=false;
    assert(!db.open());
    assert(db.openFailedTransient());
    available=true;
    assert(db.open());
    assert(slot(db.fixed(ItemIconDb::kQuestionMark,1))==0);
    failMapping=true;
    assert(slot(db.byDisplay(10,1))==0);
    const unsigned failedReads=reads;
    assert(slot(db.byDisplay(10,2))==0 && reads==failedReads);
    now+=1100;
    assert(slot(db.byDisplay(10,3))==14); // transient failure did not poison the display mapping
    failIcon=true;
    assert(slot(db.byDisplay(11,4))==0);
    const unsigned iconFailureReads=reads;
    assert(slot(db.byDisplay(11,5))==0 && reads==iconFailureReads);
    now+=2100;
    assert(slot(db.byDisplay(11,6))==15); // blank cached icon is retried
    // A bag, equipment and merchant panel can exceed the old 48-icon cache.
    for (uint32_t id=1;id<=70;++id) assert(slot(db.byDisplay(id,20))==id+4);
    const unsigned warmReads=reads;
    for (uint32_t id=1;id<=70;++id) assert(slot(db.byDisplay(id,21))==id+4);
    assert(reads==warmReads);
    now+=4000;
    db.tick(now);
    assert(openFiles==0);
    tight=true;
    // New mapping and pixels each release their file immediately; cached
    // icons remain visible and need no additional reads.
    assert(slot(db.byDisplay(80,22))==84);
    assert(openFiles==0);
    const unsigned pressureReads=reads;
    assert(slot(db.byDisplay(80,23))==84 && reads==pressureReads);
    std::puts("Item icon reader recovers mapping/texture failures and keeps 70 simultaneous icons visible.");
}
