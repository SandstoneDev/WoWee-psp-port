#include "platform/psp/psp_audio.hpp"
#include "platform/psp/psp_audio_io.hpp"
#include "platform/psp/psp_audio_math.hpp"

#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"  // the descriptor budget counts the track's

#include <pspaudio.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspmp3.h>
#include <psputility.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wowee::platform::psp {

namespace {

constexpr int kBlock = 1024;                 // stereo frames per sceAudioOutputBlocking
constexpr uint32_t kOutRate = 44100;
constexpr uint32_t kRingFrames = 48 * 1024;  // ~1.1 s of stereo s16 (192 KB)
constexpr int kVoices = 12;

// --- music: request (main thread) -> decoder thread ---------------------
struct MusicRequest {
    volatile uint32_t gen = 0;
    char path[256] = {};
    volatile bool loop = false;
    volatile bool stop = false;
    volatile uint32_t fadeOutMs = 1000, fadeInMs = 1000;
};
MusicRequest g_req;

// PCM ring: decoder writes, feeder reads (SPSC; each index has one owner).
int16_t g_ring[kRingFrames * 2] __attribute__((aligned(64)));
volatile uint32_t g_wr = 0, g_rd = 0;          // in frames, free-running
volatile uint32_t g_ringGen = 0;               // bumped by the decoder on a new track
volatile bool g_decoding = false;              // a track is being decoded
volatile uint32_t g_handledGen = 0;            // last request the decoder has acted on

// Fade state, owned by the feeder; the decoder only posts targets.
volatile float g_musicVolume = 1.0f;
volatile uint32_t g_fadeGen = 0;               // new fade request
volatile float g_fadeTarget = 1.0f;
volatile uint32_t g_fadeMs = 1000;
volatile float g_musicGain = 0.0f;             // current (feeder)

// a movie owns the ring (moviePcmBegin .. moviePcmEnd): the movie writes, the feeder plays it at
// g_movieGain, the decoder thread idles (requests made meanwhile wait for the end).
volatile bool g_movie = false;
volatile float g_movieGain = 1.0f;
volatile uint32_t g_moviePlayed = 0;           // feeder: frames given to the DAC in movie mode
volatile uint32_t g_movieUnderruns = 0;
volatile uint32_t g_feederRingGen = 0;         // feeder: the ring generation it has flushed to

// --- sfx voices (generation-guarded, skill: voice-steal needs guards) ----
struct Voice {
    volatile uint32_t gen = 0;       // bumped before any field changes
    volatile bool active = false;
    const int16_t* pcm = nullptr;
    uint32_t frames = 0;
    int channels = 1;
    uint32_t step = 0;               // 16.16 source frames per output frame
    uint64_t pos = 0;                // 16.16
    bool loop = false;
    uint32_t tag = 0;
    volatile bool stopping = false;
    volatile float gainL = 0.0f, gainR = 0.0f;
};
Voice g_voices[kVoices];
const int16_t* volatile g_mixingPcm = nullptr;
volatile float g_sfxVolume = 1.0f;

AudioStats g_stats;
SceUID g_decoderThread = -1, g_musicFeeder = -1, g_sfxFeeder = -1;
int g_musicCh = -1, g_sfxCh = -1;
volatile bool g_running = false;
bool g_ok = false;

uint32_t ringUsed() { return g_wr - g_rd; }

// --- decoder ------------------------------------------------------------
unsigned char g_mp3Buf[16 * 1024] __attribute__((aligned(64)));
unsigned char g_pcmBuf[16 * 1152] __attribute__((aligned(64)));

struct Track {
    SceUID fd = -1;
    int handle = -1;
    uint32_t rate = 0;
    int channels = 0;
    uint32_t step = 0;     // 16.16 source frames per output frame
    uint32_t frac = 0;     // resampler phase
    int16_t last[2] = {0, 0};
    bool loop = false;
    bool started = false;
};

void closeTrack(Track& t) {
    if (t.handle >= 0) sceMp3ReleaseMp3Handle(t.handle);
    if (t.fd >= 0) {
        pakNoteClose(t.fd);
        sceIoClose(t.fd);
    }
    t = Track{};
}

bool feedStream(Track& t) {
    if (sceMp3CheckStreamDataNeeded(t.handle) <= 0) return true;
    SceUChar8* dst = nullptr;
    SceInt32 toWrite = 0, srcPos = 0;
    if (sceMp3GetInfoToAddStreamData(t.handle, &dst, &toWrite, &srcPos) < 0) return false;
    if (sceIoLseek32(t.fd, srcPos, PSP_SEEK_SET) < 0) return false;
    const int got = sceIoRead(t.fd, dst, toWrite);
    if (got < 0) return false;
    return sceMp3NotifyAddStreamData(t.handle, got) >= 0;
}

// PPSSPP "music_mp3_failed handle=0" (works on hardware).
// PPSSPP's HLE sceMp3Init (Core/HLE/sceMp3.cpp) looks for the first frame
// sync only in the first 1440 bytes the game fed (FindMp3Header) and returns
// SCE_AVCODEC_ERROR_INVALID_DATA (0x807F00FD) past that: an ID3v2 tag larger
// than that (cover art, padding) or a false sync inside a tag fails there.
// It also refuses a sample rate other than 44.1 kHz from a program that
// declares no SDK version (0x80671302; PPSSPP 1.17-1.19: any version below
// "3090500"; this EBOOT declares none - libmp3.prx on hardware makes the same
// check against 0x030904FF). Handle 0 is simply the first handle.
// The fix keeps the hardware order untouched and only acts when that first
// sceMp3Init failed: the stream is reserved again starting at the first
// frame header after the ID3v2 tag (the file is scanned here: two
// consecutive valid headers). A rate refusal cannot be fixed without
// declaring an SDK version (changes firmware behaviour everywhere) and is
// logged as such (hint=rate_not_44100).

// MPEG audio frame header -> frame length in bytes (0 = not a layer III header).
uint32_t mp3FrameBytes(const uint8_t* p) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
    const int version = (p[1] >> 3) & 3;  // 3 MPEG1, 2 MPEG2, 0 MPEG2.5, 1 reserved
    const int layer = (p[1] >> 1) & 3;    // 1 = layer III
    const int brIdx = p[2] >> 4, srIdx = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
    if (version == 1 || layer != 1 || brIdx == 0 || brIdx == 15 || srIdx == 3) return 0;
    static const uint16_t kBr1[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const uint16_t kBr2[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    static const uint16_t kSr[3] = {44100, 48000, 32000};
    const uint32_t br = (version == 3 ? kBr1[brIdx] : kBr2[brIdx]) * 1000u;
    uint32_t sr = kSr[srIdx];
    if (version == 2) sr /= 2;
    else if (version == 0) sr /= 4;
    const uint32_t samples = version == 3 ? 1152u : 576u;
    return samples / 8u * br / sr + static_cast<uint32_t>(pad);
}

// Where the audio starts: past an ID3v2 tag, at the first of two consecutive
// frame headers within 64 KB. 0 = at the very start (nothing to skip) or none.
uint32_t mp3AudioStart(SceUID fd, SceOff size) {
    static uint8_t buf[8 * 1024] __attribute__((aligned(64)));
    uint32_t pos = 0;
    if (sceIoLseek32(fd, 0, PSP_SEEK_SET) != 0 || sceIoRead(fd, buf, 10) != 10) return 0;
    if (buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3') {
        pos = 10u + ((buf[6] & 0x7Fu) << 21 | (buf[7] & 0x7Fu) << 14 | (buf[8] & 0x7Fu) << 7 | (buf[9] & 0x7Fu));
        if (buf[5] & 0x10) pos += 10;  // footer present
    }
    for (uint32_t scanned = 0; scanned < 64u * 1024u && static_cast<SceOff>(pos) + 8 < size;) {
        if (sceIoLseek32(fd, static_cast<int>(pos), PSP_SEEK_SET) != static_cast<int>(pos)) return 0;
        const int got = sceIoRead(fd, buf, sizeof(buf));
        if (got < 8) return 0;
        for (int i = 0; i + 4 <= got; ++i) {
            const uint32_t len = mp3FrameBytes(buf + i);
            if (len < 24) continue;
            const uint32_t next = pos + static_cast<uint32_t>(i) + len;
            if (static_cast<SceOff>(next) + 4 > size) continue;
            uint8_t h2[4];
            if (static_cast<uint32_t>(i) + len + 4 <= static_cast<uint32_t>(got)) {
                std::memcpy(h2, buf + i + len, 4);
            } else if (sceIoLseek32(fd, static_cast<int>(next), PSP_SEEK_SET) != static_cast<int>(next) ||
                       sceIoRead(fd, h2, 4) != 4) {
                continue;
            }
            if (mp3FrameBytes(h2)) return pos + static_cast<uint32_t>(i);
        }
        pos += static_cast<uint32_t>(got) - 3u;
        scanned += static_cast<uint32_t>(got) - 3u;
    }
    return 0;
}

bool openTrack(Track& t, const char* path, bool loop) {
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    t.fd = audioOpenRead(path,0777);
    if (t.fd < 0) {
        PSP_LOGW(tag::Sys, "music_open_failed r=%08X", static_cast<unsigned>(t.fd));
        PSP_LOGW(tag::Sys, "music_file=%s", path);
        if (static_cast<unsigned>(t.fd) == 0x80020320u) pakFdRelieve(1);  // MFILE - the pressure
        t.fd = -1;
        return false;
    }
    pakNoteOpen(t.fd, path);  // in the descriptor budget
    const SceOff size = sceIoLseek(t.fd, 0, PSP_SEEK_END);
    sceIoLseek(t.fd, 0, PSP_SEEK_SET);
    SceMp3InitArg arg{};
    arg.mp3StreamStart = 0;
    arg.mp3StreamEnd = size;
    arg.mp3Buf = g_mp3Buf;
    arg.mp3BufSize = sizeof(g_mp3Buf);
    arg.pcmBuf = g_pcmBuf;
    arg.pcmBufSize = sizeof(g_pcmBuf);
    // the same calls in the same order as before; only the result
    // of each step is kept for the log and the retry below.
    int step = 0, r = 0;
    t.handle = sceMp3ReserveMp3Handle(&arg);
    if (t.handle < 0) {
        step = 1;
        r = t.handle;
    } else if (!feedStream(t)) {
        step = 2;
    } else if ((r = sceMp3Init(t.handle)) < 0) {
        step = 3;
    }
    if (step == 3) {
        // Init refused the data: start past the ID3v2 tag / at the first frame
        // (also for a rate refusal: a false sync inside a tag parses as any rate).
        const uint32_t start = mp3AudioStart(t.fd, size);
        PSP_LOGW(tag::Sys, "music_mp3_retry r=%08X start=%u", static_cast<unsigned>(r),
                 static_cast<unsigned>(start));
        if (start > 0) {
            sceMp3ReleaseMp3Handle(t.handle);
            arg.mp3StreamStart = start;
            t.handle = sceMp3ReserveMp3Handle(&arg);
            if (t.handle < 0) {
                r = t.handle;
            } else if (!feedStream(t)) {
                r = 0;
            } else if ((r = sceMp3Init(t.handle)) >= 0) {
                step = 0;
                PSP_LOGI(tag::Sys, "music_mp3_retry_ok start=%u", static_cast<unsigned>(start));
            }
        }
    }
    if (step) {
        static const char* const kStep[] = {"", "reserve", "feed", "init"};
        PSP_LOGW(tag::Sys, "music_mp3_failed handle=%d step=%s r=%08X%s", t.handle, kStep[step],
                 static_cast<unsigned>(r), static_cast<unsigned>(r) == 0x80671302u ? " hint=rate_not_44100" : "");
        ++g_stats.decodeErrors;
        closeTrack(t);
        return false;
    }
    sceMp3SetLoopNum(t.handle, loop ? -1 : 0);
    t.rate = static_cast<uint32_t>(sceMp3GetSamplingRate(t.handle));
    t.channels = sceMp3GetMp3ChannelNum(t.handle);
    if (t.rate == 0 || t.channels < 1 || t.channels > 2) {
        PSP_LOGW(tag::Sys, "music_format_bad path=%s rate=%u ch=%d", path, static_cast<unsigned>(t.rate), t.channels);
        closeTrack(t);
        return false;
    }
    t.step = static_cast<uint32_t>((static_cast<uint64_t>(t.rate) << 16) / kOutRate);
    t.loop = loop;
    PSP_LOGI(tag::Sys, "music_open rate=%u ch=%d kbps=%d kb=%u ms=%u", static_cast<unsigned>(t.rate), t.channels,
             static_cast<int>(sceMp3GetBitRate(t.handle)), static_cast<unsigned>(size / 1024),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    return true;
}

// Decoded frames -> 44.1 kHz stereo into the ring (linear 16.16).
void pushPcm(Track& t, const int16_t* src, int frames) {
    for (int i = 0; i < frames;) {
        if (ringUsed() >= kRingFrames) return;  // should not happen: the caller checks room
        // Interpolate between `last` and src[i] at phase frac.
        const int16_t* s = src + i * t.channels;
        const int l = s[0], r = t.channels == 2 ? s[1] : s[0];
        const int f = static_cast<int>(t.frac >> 1);  // 15-bit
        const uint32_t w = g_wr % kRingFrames;
        g_ring[w * 2] = static_cast<int16_t>(t.last[0] + (((l - t.last[0]) * f) >> 15));
        g_ring[w * 2 + 1] = static_cast<int16_t>(t.last[1] + (((r - t.last[1]) * f) >> 15));
        asm volatile("" ::: "memory");
        g_wr = g_wr + 1;
        t.frac += t.step;
        while (t.frac >= 0x10000u && i < frames) {
            t.frac -= 0x10000u;
            const int16_t* c = src + i * t.channels;
            t.last[0] = c[0];
            t.last[1] = t.channels == 2 ? c[1] : c[0];
            ++i;
        }
    }
}

void setFade(float target, uint32_t ms) {
    g_fadeTarget = target;
    g_fadeMs = ms;
    g_fadeGen = g_fadeGen + 1;
}

int decoderThread(SceSize, void*) {
    Track t;
    uint32_t seenGen = 0;
    // Pending request applied once the fade-out is done.
    bool pending = false;
    uint64_t fadeDoneUs = 0;
    while (g_running) {
        if (g_movie) {  // the ring is the movie's (moviePcmBegin waited for this thread to idle)
            sceKernelDelayThread(10 * 1000);
            continue;
        }
        if (g_req.gen != seenGen) {
            seenGen = g_req.gen;
            pending = true;
            const uint32_t out = g_decoding || ringUsed() ? g_req.fadeOutMs : 0;
            setFade(0.0f, out);
            fadeDoneUs = sceKernelGetSystemTimeWide() + static_cast<uint64_t>(out) * 1000u;
        }
        if (pending && static_cast<uint64_t>(sceKernelGetSystemTimeWide()) >= fadeDoneUs) {
            pending = false;
            g_handledGen = seenGen;
            closeTrack(t);
            g_decoding = false;
            g_ringGen = g_ringGen + 1;  // the feeder drops what is left
            if (!g_req.stop && g_req.path[0] && openTrack(t, g_req.path, g_req.loop)) {
                g_decoding = true;
                setFade(1.0f, g_req.fadeInMs);
            }
        }
        if (!g_decoding || pending) {
            sceKernelDelayThread(10 * 1000);
            continue;
        }
        // Room for one decoded MP3 frame (1152 source frames, up to 2x upsampled).
        if (kRingFrames - ringUsed() < 2 * 1152 + 16) {
            sceKernelDelayThread(15 * 1000);
            continue;
        }
        if (!feedStream(t)) {
            ++g_stats.decodeErrors;
            PSP_LOGW(tag::Sys, "music_read_failed");
            closeTrack(t);
            g_decoding = false;
            continue;
        }
        SceShort16* pcm = nullptr;
        const int bytes = sceMp3Decode(t.handle, &pcm);
        if (bytes <= 0) {
            // 0 / 0x80671402 = end of stream (a looping handle rewinds itself).
            if (bytes < 0 && static_cast<unsigned>(bytes) != 0x80671402u) ++g_stats.decodeErrors;
            PSP_LOGI(tag::Sys, "music_end r=%08X", static_cast<unsigned>(bytes));
            closeTrack(t);
            g_decoding = false;
            continue;
        }
        if (!t.started) { t.started=true; ++g_stats.musicStarts; }
        pushPcm(t, pcm, bytes / (2 * t.channels));
    }
    closeTrack(t);
    return 0;
}

// --- feeders ------------------------------------------------------------
int16_t g_musicOut[2][kBlock * 2] __attribute__((aligned(64)));
int16_t g_sfxOut[2][kBlock * 2] __attribute__((aligned(64)));
int32_t g_mix[kBlock * 2];

int musicFeeder(SceSize, void*) {
    unsigned outputIndex=0;
    uint32_t seenRingGen = 0, seenFade = 0;
    float gain = 0.0f, target = 0.0f, stepPerBlock = 1.0f;
    while (g_running) {
        auto& output=g_musicOut[outputIndex];
        if (g_ringGen != seenRingGen) {
            seenRingGen = g_ringGen;
            g_rd = g_wr;  // flush: the decoder has moved on
        }
        g_feederRingGen = seenRingGen;  // moviePcmBegin waits for the flush
        if (g_movie) {  // the movie's PCM at its own gain, no fades
            if (ringUsed() >= static_cast<uint32_t>(kBlock)) {
                const int gi = static_cast<int>(g_movieGain * 32768.0f);
                for (int i = 0; i < kBlock; ++i) {
                    const uint32_t r = (g_rd + i) % kRingFrames;
                    output[i * 2] = static_cast<int16_t>((g_ring[r * 2] * gi) >> 15);
                    output[i * 2 + 1] = static_cast<int16_t>((g_ring[r * 2 + 1] * gi) >> 15);
                }
                asm volatile("" ::: "memory");
                g_rd = g_rd + kBlock;
                g_moviePlayed = g_moviePlayed + kBlock;
            } else {
                g_movieUnderruns = g_movieUnderruns + 1;
                std::memset(output, 0, sizeof(output));
            }
            g_stats.ringFill = ringUsed();
            sceKernelDcacheWritebackRange(output,sizeof(output));
            if (sceAudioOutputBlocking(g_musicCh, PSP_AUDIO_VOLUME_MAX, output)<0) ++g_stats.outputErrors;
            outputIndex^=1u;
            continue;
        }
        if (g_fadeGen != seenFade) {
            seenFade = g_fadeGen;
            target = g_fadeTarget;
            const float blocks = std::max(1.0f, g_fadeMs * (kOutRate / 1000.0f) / kBlock);
            stepPerBlock = std::max(1e-4f, std::fabs(target - gain) / blocks);
        }
        const float g0 = gain;
        if (gain < target) gain = std::min(target, gain + stepPerBlock);
        else if (gain > target) gain = std::max(target, gain - stepPerBlock);
        g_musicGain = gain;
        const uint32_t used = ringUsed();
        const float vol = g_musicVolume * 0.4f;  // headroom for the separate SFX hardware channel
        if (used >= static_cast<uint32_t>(kBlock) && (gain > 0.0f || g0 > 0.0f)) {
            const float a = g0 * vol * 32768.0f, b = gain * vol * 32768.0f;
            for (int i = 0; i < kBlock; ++i) {
                const uint32_t r = (g_rd + i) % kRingFrames;
                const int gi = static_cast<int>(a + (b - a) * (static_cast<float>(i) / kBlock));
                output[i * 2] = static_cast<int16_t>((g_ring[r * 2] * gi) >> 15);
                output[i * 2 + 1] = static_cast<int16_t>((g_ring[r * 2 + 1] * gi) >> 15);
            }
            asm volatile("" ::: "memory");
            g_rd = g_rd + kBlock;
        } else {
            if (used >= static_cast<uint32_t>(kBlock)) g_rd = g_rd + kBlock;  // silent: keep the ring moving
            else if (g_decoding) ++g_stats.underruns;
            std::memset(output, 0, sizeof(output));
        }
        g_stats.ringFill = ringUsed();
        sceKernelDcacheWritebackRange(output,sizeof(output));
            if (sceAudioOutputBlocking(g_musicCh, PSP_AUDIO_VOLUME_MAX, output)<0) ++g_stats.outputErrors;
            outputIndex^=1u;
    }
    return 0;
}

int sfxFeeder(SceSize, void*) {
    unsigned outputIndex=0;
    int mixGain = 32768;
    while (g_running) {
        auto& output=g_sfxOut[outputIndex];
        const uint64_t mixStart=sceKernelGetSystemTimeWide();
        std::memset(g_mix, 0, sizeof(g_mix));
        uint32_t playing = 0;
        const float cat = g_sfxVolume;
        for (Voice& v : g_voices) {
            if (!v.active) continue;
            const uint32_t gen = v.gen;
            // Snapshot (the main thread may restart the voice meanwhile).
            const int16_t* pcm = v.pcm;
            g_mixingPcm=pcm;
            asm volatile("" ::: "memory");
            const uint32_t frames = v.frames, step = v.step;
            const int ch = v.channels;
            uint64_t pos = v.pos;
            const bool loop = v.loop;
            const uint32_t overlap=loop ? loopCrossfadeFrames(frames,static_cast<uint32_t>((static_cast<uint64_t>(step)*kOutRate)>>16)) : 0u;
            const uint32_t loopFrames=frames-overlap;
            const uint32_t fadeScale=overlap ? (1u<<24)/overlap : 0u;
            const bool stopping=v.stopping;
            const int gl = static_cast<int>(v.gainL * cat * 32768.0f), gr = static_cast<int>(v.gainR * cat * 32768.0f);
            if (!pcm || frames == 0) continue;
            ++playing;
            bool ended = false;
            for (int i = 0; i < kBlock; ++i) {
                uint32_t idx = static_cast<uint32_t>(pos >> 16);
                if (idx >= frames) {
                    if (!loop) {
                        ended = true;
                        break;
                    }
                    const uint64_t start=static_cast<uint64_t>(overlap)<<16;
                    pos=start+(pos-(static_cast<uint64_t>(frames)<<16))%(static_cast<uint64_t>(loopFrames)<<16);
                    idx = static_cast<uint32_t>(pos >> 16);
                }
                if (stopping && i>=128) { ended=true; break; }
                const uint32_t next=idx+1<frames ? idx+1 : idx;
                const int16_t* s=pcm+idx*ch;
                const int16_t* t=pcm+next*ch;
                const uint32_t fraction=static_cast<uint32_t>(pos & 0xFFFFu);
                int l=interpolatePcm(s[0],t[0],fraction);
                int r=ch==2 ? interpolatePcm(s[1],t[1],fraction) : l;
                if (overlap && idx>=frames-overlap) {
                    const uint32_t head=idx-(frames-overlap);
                    const int16_t* a=pcm+head*ch;
                    const int16_t* b=a+ch;
                    const int hl=interpolatePcm(a[0],b[0],fraction);
                    const int hr=ch==2 ? interpolatePcm(a[1],b[1],fraction) : hl;
                    l=loopCrossfadePcm(l,hl,head,fadeScale);
                    r=loopCrossfadePcm(r,hr,head,fadeScale);
                }
                int envelope=128;
                if (!loop) envelope=static_cast<int>(std::min<uint32_t>(128,std::min(idx,frames-idx)));
                else if (overlap>=128u) envelope=static_cast<int>(std::min<uint32_t>(128,idx));
                if (stopping) envelope=std::min(envelope,127-i);
                g_mix[i*2]+=((l*gl)>>15)*envelope/128;
                g_mix[i*2+1]+=((r*gr)>>15)*envelope/128;
                pos += step;
            }
            if (v.gen == gen) {  // not restarted meanwhile
                v.pos = pos;
                if (ended) v.active = false;
            }
        }
        g_mixingPcm=nullptr;
        g_stats.sfxVoices = playing;
        int32_t peak = 0;
        for (int i = 0; i < kBlock * 2; ++i) peak = std::max(peak, std::abs(g_mix[i]));
        mixGain = mixBlockGain(peak, mixGain);
        for (int i = 0; i < kBlock * 2; ++i) output[i] = scaleMixedPcm(g_mix[i], mixGain);
        // The submitted block remains owned by the audio device. Fill the other
        // buffer next (the same ping-pong scheme used by PSPSDK pspaudiolib).
        sceKernelDcacheWritebackRange(output,sizeof(output));
        const uint32_t mixUs=static_cast<uint32_t>(sceKernelGetSystemTimeWide()-mixStart);
        g_stats.sfxMixMaxUs=std::max(g_stats.sfxMixMaxUs,mixUs);
        if (mixUs>static_cast<uint32_t>(kBlock)*1000000u/kOutRate) ++g_stats.sfxLateBlocks;
        if (sceAudioOutputBlocking(g_sfxCh, PSP_AUDIO_VOLUME_MAX, output)<0) ++g_stats.outputErrors;
        outputIndex^=1u;
    }
    return 0;
}

} // namespace

bool audioInit() {
    if (g_ok) return true;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    const int m1 = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    const int m2 = sceUtilityLoadModule(PSP_MODULE_AV_MP3);
    const int res = sceMp3InitResource();
    g_musicCh = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, kBlock, PSP_AUDIO_FORMAT_STEREO);
    g_sfxCh = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, kBlock, PSP_AUDIO_FORMAT_STEREO);
    PSP_LOGI(tag::Sys, "audio_init avcodec=%08X mp3=%08X mp3res=%08X ch=%d,%d", static_cast<unsigned>(m1),
             static_cast<unsigned>(m2), static_cast<unsigned>(res), g_musicCh, g_sfxCh);
    if (g_musicCh < 0 || g_sfxCh < 0) return false;
    g_running = true;
    // Feeders first, decoder next, gameplay next: a busy render must not starve MP3 decoding.
    g_decoderThread = sceKernelCreateThread("wowee_mp3dec", decoderThread, 0x18, 32 * 1024, PSP_THREAD_ATTR_USER, nullptr);
    g_musicFeeder = sceKernelCreateThread("wowee_music", musicFeeder, 0x12, 16 * 1024, PSP_THREAD_ATTR_USER, nullptr);
    g_sfxFeeder = sceKernelCreateThread("wowee_sfx", sfxFeeder, 0x12, 16 * 1024, PSP_THREAD_ATTR_USER, nullptr);
    if (g_decoderThread < 0 || g_musicFeeder < 0 || g_sfxFeeder < 0) {
        PSP_LOGE(tag::Sys, "audio_threads_failed dec=%08X mus=%08X sfx=%08X", static_cast<unsigned>(g_decoderThread),
                 static_cast<unsigned>(g_musicFeeder), static_cast<unsigned>(g_sfxFeeder));
        g_running = false;
        return false;
    }
    sceKernelStartThread(g_decoderThread, 0, nullptr);
    sceKernelStartThread(g_musicFeeder, 0, nullptr);
    sceKernelStartThread(g_sfxFeeder, 0, nullptr);
    g_ok = true;
    PSP_LOGI(tag::Sys, "audio_ready ms=%u", static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    return true;
}

void audioShutdown() {
    if (!g_ok) return;
    g_running = false;
    asm volatile("" ::: "memory");
    for (SceUID* handle : {&g_decoderThread, &g_musicFeeder, &g_sfxFeeder}) {
        const SceUID t = *handle;
        if (t < 0) continue;
        SceUInt timeout = 3000 * 1000; // allow an in-flight Memory Stick read to finish
        const int rc = sceKernelWaitThreadEnd(t, &timeout);
        if (rc < 0) {
            PSP_LOGW(tag::Exit, "audio_shutdown thread=%d wait=%08X terminating=1", t, static_cast<unsigned>(rc));
            sceKernelTerminateDeleteThread(t);
        } else {
            sceKernelDeleteThread(t);
        }
        *handle = -1;
    }
    // No mixer may retain a PCM pointer when the screen destroys its sound bank.
    g_mixingPcm = nullptr;
    for (auto& voice : g_voices) voice = {};
    g_decoding = false;
    g_movie = false;
    // OutputBlocking queues a hardware block; wait for it before releasing channels.
    uint32_t drainMs = 0;
    while ((sceAudioGetChannelRestLen(g_musicCh) > 0 || sceAudioGetChannelRestLen(g_sfxCh) > 0) && drainMs < 500) {
        sceKernelDelayThread(1000);
        ++drainMs;
    }
    if (drainMs == 500) PSP_LOGW(tag::Exit, "audio_shutdown drain_timeout=1");
    sceAudioChRelease(g_musicCh);
    sceAudioChRelease(g_sfxCh);
    g_musicCh = g_sfxCh = -1;
    sceMp3TermResource();
    g_ok = false;
}

const AudioStats& audioStats() { return g_stats; }

void musicPlay(const char* absPath, bool loop, uint32_t fadeOutMs, uint32_t fadeInMs) {
    if (!g_ok) return;
    std::strncpy(g_req.path, absPath, sizeof(g_req.path) - 1);
    g_req.path[sizeof(g_req.path) - 1] = 0;
    g_req.loop = loop;
    g_req.stop = false;
    g_req.fadeOutMs = fadeOutMs;
    g_req.fadeInMs = fadeInMs;
    asm volatile("" ::: "memory");
    g_req.gen = g_req.gen + 1;
}

void musicStop(uint32_t fadeOutMs) {
    if (!g_ok) return;
    g_req.stop = true;
    g_req.fadeOutMs = fadeOutMs;
    asm volatile("" ::: "memory");
    g_req.gen = g_req.gen + 1;
}

void musicSetVolume(float v) { g_musicVolume = std::clamp(v, 0.0f, 1.0f); }

bool musicActive() {
    // A completed track can leave fade gain at 1 forever. Gain is not playback state.
    return g_ok && (g_req.gen != g_handledGen || g_decoding || ringUsed() >= static_cast<uint32_t>(kBlock));
}

int sfxPlay(const int16_t* pcm, uint32_t frames, int channels, uint32_t rate, float gain, float pan, bool loop, uint32_t tag) {
    if (!g_ok || !pcm || !frames || channels < 1 || channels > 2 || rate == 0) return -1;
    int slot = -1;
    float quietest = 2.0f;
    for (int i = 0; i < kVoices; ++i) {
        if (!g_voices[i].active) {
            slot = i;
            break;
        }
        const float g = std::max(g_voices[i].gainL, g_voices[i].gainR);
        if (!g_voices[i].loop && g < quietest) {
            quietest = g;
            slot = i;
        }
    }
    if (slot < 0 || (g_voices[slot].active && quietest >= gain)) return -1;  // all busy with louder sounds
    if (g_voices[slot].active) {
        // Never cut an audible waveform to steal a voice. Fade it and let the
        // next sound use the released slot on the following mixer block.
        g_voices[slot].stopping = true;
        return -1;
    }
    gain = std::clamp(gain, 0.0f, 1.0f);
    Voice& v = g_voices[slot];
    v.gen = v.gen + 1;  // before touching fields (the feeder re-checks it)
    v.active = false;
    asm volatile("" ::: "memory");
    v.pcm = pcm;
    v.frames = frames;
    v.channels = channels;
    v.step = static_cast<uint32_t>((static_cast<uint64_t>(rate) << 16) / kOutRate);
    v.pos = 0;
    v.loop = loop;
    v.tag = tag; v.stopping = false;
    const float p = std::clamp(pan, -1.0f, 1.0f);
    v.gainL = gain * (p > 0.0f ? 1.0f - p : 1.0f);
    v.gainR = gain * (p < 0.0f ? 1.0f + p : 1.0f);
    asm volatile("" ::: "memory");
    v.active = true;
    return slot;
}

void sfxStop(int voice) {
    if (voice < 0 || voice >= kVoices) return;
    g_voices[voice].gen = g_voices[voice].gen + 1;
    g_voices[voice].active = false;
}

void sfxStopTag(uint32_t tag) {
    if (!tag) return;
    for (auto& v : g_voices) if (v.active && v.tag==tag) v.stopping=true;
}

void sfxSetGain(int voice, float gain, float pan) {
    if (voice < 0 || voice >= kVoices) return;
    const float p = std::clamp(pan, -1.0f, 1.0f);
    g_voices[voice].gainL = gain * (p > 0.0f ? 1.0f - p : 1.0f);
    g_voices[voice].gainR = gain * (p < 0.0f ? 1.0f + p : 1.0f);
}

void sfxSetVolume(float v) { g_sfxVolume = std::clamp(v, 0.0f, 1.0f); }

// sounds - before the PCM of every voice is freed (SoundBank::close).
void sfxStopAll() {
    for (int i = 0; i < kVoices; ++i) sfxStop(i);
}

bool sfxUsesPcm(const int16_t* pcm) {
    if (g_mixingPcm==pcm) return true;
    for (const auto& voice:g_voices) if (voice.active && voice.pcm==pcm) return true;
    return false;
}

bool sfxPlayingTag(int voice, uint32_t tag) { return voice>=0 && voice<kVoices && g_voices[voice].active && !g_voices[voice].stopping && g_voices[voice].tag==tag; }

bool sfxPlaying(int voice) { return voice >= 0 && voice < kVoices && g_voices[voice].active; }

// the movie's sound through the music channel (psp_audio.hpp).
bool moviePcmBegin(float gain) {
    if (!g_ok) return false;
    musicStop(0);
    // No fade at 0 ms: the decoder closes the track on its next pass (~10 ms); then it idles while g_movie is set.
    for (int i = 0; i < 60 && (g_req.gen != g_handledGen || g_decoding); ++i) sceKernelDelayThread(5 * 1000);
    g_movieGain = std::clamp(gain, 0.0f, 1.0f);
    g_moviePlayed = 0;
    g_movieUnderruns = 0;
    g_movie = true;
    asm volatile("" ::: "memory");
    const uint32_t gen = g_ringGen + 1;
    g_ringGen = gen;  // the feeder (it owns the read index) empties the ring before the movie writes
    for (int i = 0; i < 100 && g_feederRingGen != gen; ++i) sceKernelDelayThread(2 * 1000);
    if (g_feederRingGen != gen) {
        PSP_LOGW(tag::Sys, "movie_pcm_begin feeder_late gen=%u seen=%u", static_cast<unsigned>(gen),
                 static_cast<unsigned>(g_feederRingGen));
        g_movie = false;
        return false;
    }
    return true;
}

uint32_t moviePcmRoom() { return g_movie ? kRingFrames - ringUsed() : 0u; }

uint32_t moviePcmQueued() { return g_movie ? ringUsed() : 0u; }

bool moviePcmPush(const int16_t* pcm, uint32_t frames) {
    if (!g_movie || frames > kRingFrames - ringUsed()) return false;
    for (uint32_t i = 0; i < frames;) {
        const uint32_t w = g_wr % kRingFrames;
        const uint32_t n = std::min(frames - i, kRingFrames - w);
        std::memcpy(&g_ring[w * 2], pcm + i * 2, n * 4u);
        i += n;
        asm volatile("" ::: "memory");
        g_wr = g_wr + n;
    }
    return true;
}

uint32_t moviePcmPlayed() { return g_moviePlayed; }

uint32_t moviePcmUnderruns() { return g_movieUnderruns; }

void moviePcmEnd() {
    if (!g_movie) return;
    g_movie = false;
    asm volatile("" ::: "memory");
    g_ringGen = g_ringGen + 1;  // the feeder drops what is left; the decoder acts on the next request
}

} // namespace wowee::platform::psp
