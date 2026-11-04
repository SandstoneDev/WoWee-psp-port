#pragma once
#include <cstdint>
namespace wowee::platform::psp {
struct PakEntry { const char* name; uint32_t offset, size; };
class Pak {
public:
    bool open(const char*);
    void close() {}
    const PakEntry* find(const char*);
    void* load(const PakEntry*);
};
void pakFree(void*);
int pakOpenRetry(const char*);
void pakNoteOpen(int,const char*);
void pakNoteClose(int);
bool pakRead(int,uint32_t,void*,uint32_t);
bool pakFdTight();
}
