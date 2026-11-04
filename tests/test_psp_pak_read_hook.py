from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/platform/psp/psp_pak.cpp').read_text()
def function(signature):
    start=source.index(signature);body=source.index('{',start);end=body+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]+'\n'
cpp=r'''
#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <thread>
using SceUID=int;using PakReadTick=void(*)(void*);
PakReadTick g_readTick=nullptr;void* g_readTickUser=nullptr;std::atomic<SceUID> g_readTickThread{-1};
thread_local int threadId=1;
int sceKernelGetThreadId(){return threadId;}
void ioLock(){}void ioUnlock(){}
constexpr int PSP_SEEK_SET=0;
int sceIoLseek32(int,int offset,int){return offset;}
int sceIoRead(int,void* p,uint32_t size){std::memset(p,0,size);return static_cast<int>(size);}
'''
main=r'''
int main(){
    unsigned calls=0;char buffer[4096];
    pakSetReadTick([](void* p){assert(threadId==1);++*static_cast<unsigned*>(p);},&calls);
    assert(pakRead(0,0,buffer,sizeof(buffer))&&calls==1);
    std::thread worker([&]{threadId=2;for(int i=0;i<100;++i)assert(pakRead(0,0,buffer,sizeof(buffer)));});worker.join();
    assert(calls==1);pakSetReadTick(nullptr,nullptr);assert(pakRead(0,0,buffer,sizeof(buffer))&&calls==1);
    std::puts("Pak loading hook passed: main receives keepalive callbacks; workers never touch UI/network hooks.");
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-pak-hook-') as temp:
    folder=Path(temp);src=folder/'hook.cpp';exe=folder/'hook.exe';src.write_text(cpp+function('bool pakRead(')+function('void pakSetReadTick(')+main)
    if Path(compiler).stem.lower()=='cl':args=[compiler,'/nologo','/std:c++20','/EHsc','/O2',str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'hook.obj')]
    else:args=[compiler,'-std=c++20','-pthread','-O2',str(src),'-o',str(exe)]
    subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
