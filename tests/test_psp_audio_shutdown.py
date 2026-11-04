"""Execute the production shutdown routine against a simulated PSP kernel.

The decoder/mixer cannot run on the host. This exercises their shutdown API
boundary: joins, timeout fallback, PCM lifetime, hardware drain, and repeat calls.
Run under a C++ compiler environment: python tests/test_psp_audio_shutdown.py [cl|c++]
"""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/platform/psp/psp_audio.cpp').read_text()
routine = source.split('void audioShutdown() {', 1)[1].split('const AudioStats& audioStats()', 1)[0]
routine = 'void audioShutdown() {' + routine
# Replace only the architecture/compiler-specific memory barrier for the host.
routine = routine.replace('asm volatile("" ::: "memory");', 'std::atomic_signal_fence(std::memory_order_seq_cst);')
stub = r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <cstdio>
using SceUID=int; using SceUInt=unsigned;
#define PSP_LOGW(...) ((void)0)
bool g_ok=true, g_running=true, g_decoding=true, g_movie=true;
int g_decoderThread=1,g_musicFeeder=2,g_sfxFeeder=3,g_musicCh=4,g_sfxCh=5;
struct Voice { const int16_t* pcm=nullptr; bool active=false; };
Voice g_voices[12];
const int16_t* g_mixingPcm=nullptr;
bool stopped[4]={};
int waits=0,deletes=0,terminates=0,releases=0,codec=0,delay=0,timeoutId=-1;
bool stuck=false;
int sceKernelWaitThreadEnd(int t,SceUInt* timeout) {
    assert(!g_running && *timeout>=500000); ++waits;
    // PCM cannot be cleared before the feeders have stopped.
    assert(g_voices[0].active && g_mixingPcm);
    if(t==timeoutId) return -1;
    stopped[t]=true; return 0;
}
int sceKernelDeleteThread(int t) { assert(stopped[t]); ++deletes; return 0; }
int sceKernelTerminateDeleteThread(int t) { stopped[t]=true; ++terminates; return 0; }
int sceAudioGetChannelRestLen(int ch) {
    assert(ch==4 || ch==5); assert(stopped[1]&&stopped[2]&&stopped[3]);
    return stuck || delay<3 ? 100 : 0;
}
int sceKernelDelayThread(int us) { assert(us==1000); ++delay; return 0; }
int sceAudioChRelease(int ch) {
    assert(ch==4 || ch==5); assert(stopped[1]&&stopped[2]&&stopped[3]);
    assert(!g_voices[0].active && !g_mixingPcm); assert(delay>=3); ++releases; return 0;
}
int sceMp3TermResource() { assert(releases==2); ++codec; return 0; }
'''
main = r'''
int main() {
    static int16_t sample=8;
    for(int mode=0;mode<3;++mode) {
        g_ok=g_running=g_decoding=g_movie=true;
        g_decoderThread=1;g_musicFeeder=2;g_sfxFeeder=3;g_musicCh=4;g_sfxCh=5;
        for(auto& s:stopped) s=false;
        waits=deletes=terminates=releases=codec=delay=0;
        timeoutId=mode==1 ? 1 : -1; stuck=mode==2;
        g_voices[0]={&sample,true};g_mixingPcm=&sample;
        audioShutdown();
        assert(!g_ok&&!g_running&&!g_decoding&&!g_movie);
        assert(g_decoderThread==-1&&g_musicFeeder==-1&&g_sfxFeeder==-1);
        assert(g_musicCh==-1&&g_sfxCh==-1);
        assert(waits==3&&deletes+terminates==3&&terminates==(mode==1));
        assert(releases==2&&codec==1&&delay==(stuck?500:3));
        audioShutdown(); // Already stopped / audio disabled: no kernel calls.
        assert(waits==3&&releases==2&&codec==1);
    }
    puts("Audio shutdown passed: joins, timeout fallback, PCM lifetime, drain, stalled driver, idempotence.");
}
'''
compiler = sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-audio-shutdown-') as temp:
    folder=Path(temp)
    cpp=folder/'shutdown.cpp'; exe=folder/'shutdown.exe'
    cpp.write_text(stub+routine+main)
    if Path(compiler).stem.lower()=='cl':
        args=[compiler,'/nologo','/std:c++20','/EHsc',str(cpp),'/Fe:'+str(exe),'/Fo:'+str(folder/'shutdown.obj')]
    else:
        args=[compiler,'-std=c++20',str(cpp),'-o',str(exe)]
    subprocess.run(args,cwd=temp,check=True)
    subprocess.run([str(exe)],check=True)
