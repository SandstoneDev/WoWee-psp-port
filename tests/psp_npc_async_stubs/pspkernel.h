#pragma once
#include <cstddef>
using SceUInt=unsigned int;
constexpr int PSP_THREAD_ATTR_USER=0;
int sceKernelCreateSema(const char*,int,int,int,void*);
int sceKernelSignalSema(int,int);
int sceKernelWaitSema(int,int,void*);
int sceKernelDeleteSema(int);
int sceKernelCreateThread(const char*,int(*)(unsigned int,void*),int,unsigned int,int,void*);
int sceKernelStartThread(int,unsigned int,void*);
int sceKernelWaitThreadEnd(int,SceUInt*);
int sceKernelDeleteThread(int);
void sceKernelDcacheWritebackRange(void*,unsigned int);
void* npcTestAlloc(std::size_t,std::size_t);
void npcTestFree(void*);
#define memalign npcTestAlloc
