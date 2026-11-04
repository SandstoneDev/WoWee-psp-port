#pragma once
#include <cstdint>
namespace wowee::platform::psp {
struct PakEntry {const char* name=nullptr;uint32_t offset=0,size=0,kind=0;};
class Pak {public:bool isOpen()const{return true;}const PakEntry* find(const char*)const;};
class PakIndex {
public:bool open(const char*);void close();void releaseFd();bool find(const char*,PakEntry&);bool findOnFd(const char*,PakEntry&,int);
    bool isOpen()const{return open_;}int fd()const{return fd_;}bool lookupIoFailed()const{return ioFailed_;}
private:int fd_=-1;bool ioFailed_=false,open_=false;
};
int pakOpenRetry(const char*);void pakNoteOpen(int,const char*);void pakNoteClose(int);
int pakFdRelieve(int);
bool pakRead(int,uint32_t,void*,uint32_t);
}
