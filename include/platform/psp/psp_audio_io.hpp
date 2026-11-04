#pragma once
#include "platform/psp/psp_pak.hpp"
#include <pspiofilemgr.h>
#include <pspthreadman.h>
namespace wowee::platform::psp {
// Audio workers only: let lower-priority readers finish/return their descriptor
// and the main thread service pressure before dropping a new clip/track.
inline int audioOpenRead(const char* path,int mode=0) {
    int fd=-1;
    for(int attempt=0;attempt<4;++attempt) {
        fd=sceIoOpen(path,PSP_O_RDONLY,mode);
        if(static_cast<unsigned>(fd)!=0x80020320u) return fd;
        pakFdRelieve(1);
        if(attempt<3) sceKernelDelayThread(10000);
    }
    return fd;
}
}
