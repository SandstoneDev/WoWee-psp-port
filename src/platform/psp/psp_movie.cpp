// the PSMF movie player (include/platform/psp/psp_movie.hpp).
//
// The calls and their order follow the firmware library's documented use (open pspsdk pspmpeg.h; the pattern of
// Sony's own movie samples): load the AV modules -> sceMpegInit -> ring buffer + sceMpegCreate -> the PSMF
// header's stream offset / size -> register the AVC and ATRAC streams -> per pass: put packs into the ring (our
// callback reads the card), take access units (sceMpegGet*Au), decode them (sceMpegAvcDecode into a frame buffer,
// sceMpegAtracDecode into a PCM block) -> at the end sceMpegAvcDecodeStop hands out the decoder's last picture.
// Facts the code relies on (each one is easy to get wrong):
//  - sceMpegAvcDecode's buffer argument is the ADDRESS of a pointer to the picture buffer, and its last argument
//    comes back 1 when a picture was written - not necessarily the access unit just given (the decoder may hold
//    one): the PTS of every decoded access unit is queued and a written picture takes the oldest one;
//  - the first sceMpegAtracDecode (init flag 1) primes the decoder: its output is dropped;
//  - an access unit without a PTS reads 0xFFFFFFFF: the previous one + one frame;
//  - 0x80618001 from sceMpegGet*Au = no whole access unit in the ring yet (read more), or the end of the stream
//    once everything was read and the ring is empty;
//  - the ring, AU and decode-mode records are larger in the firmware than in pspsdk's header (MpegRing / MpegAu /
//    MpegAvcMode below).
#include "platform/psp/psp_movie.hpp"

#include "platform/psp/psp_audio.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_input.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"  // pakRead: the one card lock (world streamer, sound loader, this)
#include "platform/psp/psp_system.hpp"
#include "platform/psp/psp_texture.hpp"

#include <malloc.h>
#include <pspctrl.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspmpeg.h>
#include <psppower.h>
#include <psputility.h>
#include <psputils.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wowee::platform::psp {

namespace {

constexpr uint32_t kPack = 2048;               // the PSMF stream's unit (an MPEG-2 program stream pack)
constexpr int kStride = 512;                   // the frame buffers' width in pixels
constexpr uint32_t kFrameBytes = kStride * kScreenH * 2;  // one RGB 565 picture (the RAM-frame path)
constexpr uint32_t kFrameTicks = 3003;         // 90 kHz ticks a frame at 29.97 fps (the PSMF Encoder's one rate)
constexpr uint32_t kErrNoData = 0x80618001u;   // sceMpegGet*Au: no whole access unit in the ring
constexpr uint32_t kErrModuleLoaded = 0x80111102u;  // sceUtilityLoadModule: already loaded
// Ring sizes in packs (~2.1 KB each with the library's records), the first that allocates. The ring must span
// the sound's lead over the picture plus the muxer's video lead (STD delay 1 s): 256 packs = 512 KB of stream,
// ~2.5 s at the movies' average 1.6 Mbit/s, ~1.4 s at their 3 Mbit/s peak.
constexpr int kRingTry[] = {256, 192, 128, 64};
constexpr int kPutMin = 32, kPutMax = 64;      // packs a card read (a request costs ~1 ms whatever its size)
constexpr int kAudioPerPass = 6;               // ATRAC access units a pass at most (2048 frames = 46 ms each)
constexpr uint32_t kDacLagFrames = 1024;       // the feeder counts a block before the DAC has played it
constexpr uint32_t kAudioLeadFrames = 33075;   // decode the sound at most 0.75 s ahead (the ring holds 1.1 s)

uint64_t nowUs() { return sceKernelGetSystemTimeWide(); }

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

struct Reader {
    SceUID fd = -1;
    uint32_t pos = 0, end = 0;  // the stream's next pack and its end (file offsets)
    uint32_t reads = 0, failures = 0;
};

// sceMpegRingbufferPut's callback (the caller's thread): packs from the card straight into the ring.
SceInt32 ringFill(ScePVoid data, SceInt32 packets, ScePVoid param) {
    auto* r = static_cast<Reader*>(param);
    const uint32_t left = r->end > r->pos ? r->end - r->pos : 0u;
    uint32_t bytes = std::min<uint32_t>(static_cast<uint32_t>(packets) * kPack, left);
    bytes -= bytes % kPack;
    if (bytes == 0) return 0;
    if (!pakRead(r->fd, r->pos, data, bytes)) {
        ++r->failures;
        return -1;
    }
    r->pos += bytes;
    ++r->reads;
    return static_cast<SceInt32>(bytes / kPack);
}

alignas(64) uint8_t g_header[kPack];

// The firmware's records are larger than pspsdk's declarations (the firmware's SceMpegRingbuffer is 32 words,
// SceMpegAu 64 bytes - 24 used, 40 reserved -, SceMpegAvcMode 16 bytes with 8 reserved; pspsdk declares 44 / 24 / 8:
// PPSSPP already writes a 12th ring word, the gp, on library 0105+). The library may use the whole record: the
// pspsdk struct at the front of a zeroed record of the real size, never a bare pspsdk struct next to other data.
union MpegRing {
    SceMpegRingbuffer rb;
    uint32_t raw[32];
};
union MpegAu {
    SceMpegAu au;
    uint8_t raw[64];
};
struct MpegAvcMode {
    SceInt32 decodeMode;   // -1: no change
    SceInt32 pixelFormat;  // 0 RGB565, 1 RGBA5551, 2 RGBA4444, 3 RGBA8888
    uint8_t reserved[8];
};
static_assert(sizeof(MpegRing) == 128 && sizeof(MpegAu) == 64 && sizeof(MpegAvcMode) == 16, "libmpeg record sizes");

class Movie {
public:
    ~Movie() { teardown(); }
    MovieEnd play(const char* path, int volume, const MovieHooks& hooks);

private:
    bool open(const char* path, int volume);
    bool fail(const char* step, int r);
    void teardown();
    void* drawTarget() const;  // where the next picture is decoded to
    void present(const MovieHooks& hooks, uint32_t pts);
    void drawRamFrame();
    uint32_t clock();

    Reader reader_;
    int avcodecLoad_ = -1, mpegLoad_ = -1;
    bool mpegInit_ = false, ringBuilt_ = false, created_ = false;
    void* mpegMem_ = nullptr;
    void* ringMem_ = nullptr;
    void* atracEs_ = nullptr;
    int16_t* pcm_ = nullptr;
    void* ramFrame_ = nullptr;  // wowee.cfg moviedirect=0: decode into RAM, the GE copies it into the frame
    int mpegMemSize_ = 0, ringPackets_ = 0, ringBytes_ = 0, pcmBytes_ = 0;
    MpegRing ring_{};
    SceMpeg mpeg_ = nullptr;
    SceMpegStream* avc_ = nullptr;
    SceMpegStream* atrac_ = nullptr;
    void* avcEs_ = nullptr;
    MpegAu avcAu_{}, atracAu_{};
    bool direct_ = true, sound_ = false, decodedAny_ = false, decodeStopped_ = false, rgb565_ = false;
    uint32_t firstTs_ = 0, lastTs_ = 0, fileKb_ = 0;
    // The clock: the movie's sound (frames the DAC took) from its first block's PTS; without sound (or after it
    // ended) the system clock from the last value.
    bool audioAnchored_ = false, audioDone_ = true;
    uint32_t audioPts0_ = 0, clockHold_ = 0;
    uint64_t clockHoldUs_ = 0;
    char failStep_[24] = {};
};

const char* g_endNames[] = {"finished", "skip_next", "skip_all", "missing", "failed", "exit", "stopped"};

bool Movie::fail(const char* step, int r) {
    std::snprintf(failStep_, sizeof(failStep_), "%s", step);
    PSP_LOGE(tag::Gfx, "movie_failed step=%s r=%08X", step, static_cast<unsigned>(r));
    return false;
}

void* Movie::drawTarget() const {
    if (!direct_) return ramFrame_;
    return static_cast<uint8_t*>(sceGeEdramGetAddr()) + reinterpret_cast<uintptr_t>(displayDrawBuffer());
}

bool Movie::open(const char* path, int volume) {
    // MovieFrame_PlayMovie stops the glue music first (StopGlueMusic): here the music channel becomes the movie's -
    // which also closes the music's file before ours opens (the card driver has ~4 descriptors to spare on HW).
    sound_ = moviePcmBegin(static_cast<float>(std::clamp(volume, 0, 255)) / 255.0f);
    reader_.fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (reader_.fd < 0) {
        PSP_LOGW(tag::Gfx, "movie_missing path=%s r=%08X", path, static_cast<unsigned>(reader_.fd));
        reader_.fd = -1;
        return false;
    }
    pakNoteOpen(reader_.fd, path);  // in the descriptor budget
    const SceOff size = sceIoLseek(reader_.fd, 0, PSP_SEEK_END);
    fileKb_ = static_cast<uint32_t>(size / 1024);
    // PSMF header (one pack): "PSMF", the stream's offset and size, the first / last 90 kHz timestamps.
    if (size < static_cast<SceOff>(kPack) * 2 || !pakRead(reader_.fd, 0, g_header, kPack) ||
        std::memcmp(g_header, "PSMF", 4) != 0)
        return fail("header", static_cast<int>(size));
    firstTs_ = be32(g_header + 0x56);
    lastTs_ = be32(g_header + 0x5C);
    // The AV modules: AVCODEC is normally there already (audioInit, for sceMp3), MPEGBASE (sceMpeg) only for the
    // movie - both are unloaded after it if this loaded them (the user partition outside the heap: dialogs, net).
    avcodecLoad_ = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    if (avcodecLoad_ < 0 && static_cast<uint32_t>(avcodecLoad_) != kErrModuleLoaded) return fail("avcodec", avcodecLoad_);
    mpegLoad_ = sceUtilityLoadModule(PSP_MODULE_AV_MPEGBASE);
    if (mpegLoad_ < 0 && static_cast<uint32_t>(mpegLoad_) != kErrModuleLoaded) return fail("mpegbase", mpegLoad_);
    int r = sceMpegInit();
    if (r < 0) return fail("init", r);
    mpegInit_ = true;
    mpegMemSize_ = sceMpegQueryMemSize(0);
    if (mpegMemSize_ <= 0) return fail("mem_size", mpegMemSize_);
    mpegMem_ = memalign(64, static_cast<size_t>(mpegMemSize_));
    if (!mpegMem_) return fail("oom_work", mpegMemSize_);
    for (int packets : kRingTry) {
        ringBytes_ = sceMpegRingbufferQueryMemSize(packets);
        if (ringBytes_ <= 0) return fail("ring_size", ringBytes_);
        ringMem_ = memalign(64, static_cast<size_t>(ringBytes_));
        if (ringMem_) {
            ringPackets_ = packets;
            break;
        }
    }
    if (!ringMem_) return fail("oom_ring", ringBytes_);
    r = sceMpegRingbufferConstruct(&ring_.rb, ringPackets_, ringMem_, ringBytes_, ringFill, &reader_);
    if (r < 0) return fail("ring", r);
    ringBuilt_ = true;
    r = sceMpegCreate(&mpeg_, mpegMem_, mpegMemSize_, &ring_.rb, kStride, 0, 0);
    if (r < 0) return fail("create", r);
    created_ = true;
    MpegAvcMode mode{-1, SCE_MPEG_AVC_FORMAT_5650, {}};
    r = sceMpegAvcDecodeMode(&mpeg_, reinterpret_cast<SceMpegAvcMode*>(&mode));
    if (r < 0) return fail("decode_mode", r);
    SceInt32 offset = 0, streamSize = 0;
    r = sceMpegQueryStreamOffset(&mpeg_, g_header, &offset);
    if (r < 0 || offset < static_cast<SceInt32>(kPack)) return fail("stream_offset", r ? r : offset);
    r = sceMpegQueryStreamSize(g_header, &streamSize);
    if (r < 0 || streamSize <= 0) return fail("stream_size", r ? r : streamSize);
    reader_.pos = static_cast<uint32_t>(offset);
    reader_.end = static_cast<uint32_t>(std::min<SceOff>(size, static_cast<SceOff>(offset) + streamSize));
    avc_ = sceMpegRegistStream(&mpeg_, 0, 0);
    if (!avc_) return fail("regist_avc", 0);
    avcEs_ = sceMpegMallocAvcEsBuf(&mpeg_);
    if (!avcEs_) return fail("avc_es", 0);
    r = sceMpegInitAu(&mpeg_, avcEs_, &avcAu_.au);
    if (r < 0) return fail("avc_au_init", r);
    // The sound: a movie without one (or with the sound off, wowee.cfg audio=0) plays on the system clock.
    atrac_ = sceMpegRegistStream(&mpeg_, 1, 0);
    if (atrac_) {
        SceInt32 esSize = 0, outSize = 0;
        r = sceMpegQueryAtracEsSize(&mpeg_, &esSize, &outSize);
        if (r < 0 || esSize <= 0 || outSize <= 0) return fail("atrac_size", r);
        atracEs_ = memalign(64, static_cast<size_t>(esSize));
        pcm_ = static_cast<int16_t*>(memalign(64, static_cast<size_t>(outSize)));
        if (!atracEs_ || !pcm_) return fail("oom_atrac", esSize + outSize);
        pcmBytes_ = outSize;
        r = sceMpegInitAu(&mpeg_, atracEs_, &atracAu_.au);
        if (r < 0) return fail("atrac_au_init", r);
        // Without the sound channel (wowee.cfg audio=0) the access units are still taken and dropped: the ring
        // frees only what every registered stream has passed.
        audioDone_ = false;
    } else if (sound_) {
        moviePcmEnd();  // a movie without sound: the system clock
        sound_ = false;
    }
    // The direct VRAM decode gave wrong colours on hardware: the RAM path unless moviedirect=1.
    direct_ = configGetInt("moviedirect", 0) != 0;
    if (!direct_) {
        ramFrame_ = memalign(64, kFrameBytes);
        if (!ramFrame_) return fail("oom_frame", static_cast<int>(kFrameBytes));
    }
    return true;
}

// Teardown in the reverse order of open() (Sony's samples: free the ES buffers, unregister, delete, destruct the
// ring, finish, unload).
void Movie::teardown() {
    if (sound_) {
        moviePcmEnd();
        sound_ = false;
    }
    if (created_) {
        if (decodedAny_ && !decodeStopped_) {  // skipped: the decoder gives up the picture it holds
            void* bufs[1] = {drawTarget()};
            SceInt32 got = 0;
            sceMpegAvcDecodeStop(&mpeg_, kStride, bufs, &got);
            decodeStopped_ = true;
        }
        sceMpegFlushAllStream(&mpeg_);
        if (avcEs_) sceMpegFreeAvcEsBuf(&mpeg_, avcEs_);
        if (avc_) sceMpegUnRegistStream(&mpeg_, avc_);
        if (atrac_) sceMpegUnRegistStream(&mpeg_, atrac_);
        sceMpegDelete(&mpeg_);
        created_ = false;
    }
    avcEs_ = nullptr;
    avc_ = atrac_ = nullptr;
    if (ringBuilt_) {
        sceMpegRingbufferDestruct(&ring_.rb);
        ringBuilt_ = false;
    }
    if (mpegInit_) {
        sceMpegFinish();
        mpegInit_ = false;
    }
    for (void** p : {&mpegMem_, &ringMem_, &atracEs_, reinterpret_cast<void**>(&pcm_), &ramFrame_}) {
        if (*p) std::free(*p);
        *p = nullptr;
    }
    if (mpegLoad_ >= 0) sceUtilityUnloadModule(PSP_MODULE_AV_MPEGBASE);
    if (avcodecLoad_ >= 0) sceUtilityUnloadModule(PSP_MODULE_AV_AVCODEC);
    mpegLoad_ = avcodecLoad_ = -1;
    if (reader_.fd >= 0) {
        pakNoteClose(reader_.fd);
        sceIoClose(reader_.fd);
        reader_.fd = -1;
    }
}

// The movie's time in 90 kHz ticks. While its sound plays, the frames the DAC took; when the sound runs dry (an
// underrun: the card or the demuxer late - the picture must go on, or the ring that should bring the sound stays
// full of pictures waiting for the clock) and after the sound ended, the system clock on from the last value.
uint32_t Movie::clock() {
    if (sound_ && audioAnchored_ && (moviePcmQueued() >= kDacLagFrames || !clockHoldUs_)) {
        uint32_t played = moviePcmPlayed();
        played = played > kDacLagFrames ? played - kDacLagFrames : 0u;
        clockHold_ = audioPts0_ + static_cast<uint32_t>(static_cast<uint64_t>(played) * 90000u / 44100u);
        clockHoldUs_ = nowUs();
        return clockHold_;
    }
    if (!clockHoldUs_) return clockHold_;  // nothing shown yet: the first picture's PTS (set by play)
    return clockHold_ + static_cast<uint32_t>((nowUs() - clockHoldUs_) * 9u / 100u);
}

// One picture to the screen: a frame whose list draws only the overlay (the picture is in the colour buffer
// already, or the GE draws it there from RAM); frameEnd waits for the vblank and flips.
void Movie::present(const MovieHooks& hooks, uint32_t pts) {
    frameSkipNextClear();
    frameBegin(0);
    if (direct_) {
        if (!rgb565_) {
            // moviedirect=1: the Media Engine wrote RGB 565 into the colour buffer itself, so the GE draws (the
            // overlay) and the display shows (sceGuSwapBuffers takes Gu's pixel format) 565 from this flip on;
            // play() puts 5551 back.
            sceGuDrawBuffer(GU_PSM_5650, displayDrawBuffer(), kStride);
            rgb565_ = true;
        }
    } else {
        drawRamFrame();
    }
    if (hooks.overlay) hooks.overlay(hooks.user, pts > firstTs_ ? (pts - firstTs_) / 90u : 0u);
    frameEnd(nullptr, nullptr);
}

// PSP: the RAM picture (RGB 565, written by the Media Engine; the cache was cleaned before the decode) is drawn as
// a 565 texture into the normal 5551 colour buffer - the GE converts each texel, the display never changes format.
// Copying the 565 bits into the buffer and switching the display to 565 showed them read as 5551 (on hardware, and
// PPSSPP 1.20: green wrapped in the bright areas - pink/lime blotches over the logo and the snow).
void Movie::drawRamFrame() {
    struct Vert {
        uint16_t u, v;
        int16_t x, y, z;
    };
    constexpr int kStrip = 64;  // texels per sprite: the texture cache reads 64-px wide columns
    constexpr int kStrips = (kScreenW + kStrip - 1) / kStrip;
    auto* vtx = static_cast<Vert*>(sceGuGetMemory(kStrips * 2 * sizeof(Vert)));
    for (int i = 0; i < kStrips; ++i) {
        const int16_t x0 = static_cast<int16_t>(i * kStrip);
        const int16_t x1 = static_cast<int16_t>(std::min((i + 1) * kStrip, kScreenW));
        vtx[2 * i] = {static_cast<uint16_t>(x0), 0, x0, 0, 0};
        vtx[2 * i + 1] = {static_cast<uint16_t>(x1), static_cast<uint16_t>(kScreenH), x1,
                          static_cast<int16_t>(kScreenH), 0};
    }
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_5650, 0, 0, 0);
    sceGuTexImage(0, 512, 512, kStride, ramFrame_);  // 272 rows hold the picture; v never passes them
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFlush();
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, kStrips * 2, nullptr, vtx);
    textureResetBinding();  // the overlay binds its own textures from scratch
    sceGuEnable(GU_DEPTH_TEST);
}

MovieEnd Movie::play(const char* path, int volume, const MovieHooks& hooks) {
    const uint64_t t0 = nowUs();
    logMemory("movie_begin");
    // MovieFrame_OnShow: WorldFrame / UIParent hidden - black at once (the stream needs a moment).
    frameBegin(0xFF000000u);
    frameEnd(nullptr, nullptr);
    if (!open(path, volume)) {
        const bool missing = reader_.fd < 0;
        teardown();
        return missing ? MovieEnd::Missing : MovieEnd::Failed;
    }
    PSP_LOGI(tag::Gfx, "movie_open path=%s kb=%u stream_kb=%u first_ms=%u len_ms=%u ring_packs=%d ring_kb=%u work_kb=%u "
             "sound=%d direct=%d vol=%d", path, static_cast<unsigned>(fileKb_),
             static_cast<unsigned>((reader_.end - reader_.pos) / 1024u), static_cast<unsigned>(firstTs_ / 90u),
             static_cast<unsigned>((lastTs_ - firstTs_) / 90u), ringPackets_,
             static_cast<unsigned>(ringBytes_ / 1024), static_cast<unsigned>(mpegMemSize_ / 1024), sound_ ? 1 : 0,
             direct_ ? 1 : 0, volume);
    logMemory("movie_ready");
    MovieEnd end = MovieEnd::Finished;
    bool readDone = false, videoDone = false, stopped = false, framePending = false, atracInit = true;
    bool tailPadded = false;
    uint32_t framePts = 0, lastAuPts = firstTs_ - kFrameTicks;
    uint32_t ptsQ[16];
    int qHead = 0, qCount = 0;
    uint32_t shown = 0, dropped = 0, dropRun = 0, audioAus = 0, decoded = 0;
    uint32_t lastPlayed = 0, nextStatMs = logUptimeMs() + 10000u, powerTickMs = 0;
    uint64_t lastProgressUs = nowUs(), noDataUs = 0, drainUs = 0;
    clockHold_ = firstTs_;
    while (true) {
        const PadState& pad = inputUpdate();
        if (!systemRunning()) {
            end = MovieEnd::Exit;
            break;
        }
        // MovieFrame_OnKeyUp: ESCAPE (TOGGLEGAMEMENU) hides the frame, SPACE / ENTER stop this movie.
        if (pad.pressed & PSP_CTRL_START) {
            end = MovieEnd::SkipAll;
            break;
        }
        if (pad.pressed & PSP_CTRL_CIRCLE) {
            end = MovieEnd::SkipNext;
            break;
        }
        if (hooks.pump && !hooks.pump(hooks.user)) {
            end = MovieEnd::Stopped;
            break;
        }
        const uint32_t nowMs = logUptimeMs();
        if (nowMs >= powerTickMs) {  // no input for minutes: keep the backlight and the auto-sleep off
            scePowerTick(PSP_POWER_TICK_ALL);
            powerTickMs = nowMs + 1000u;
        }
        bool worked = false;
        // 1. The stream into the ring: a read when 32+ packs are free (the first passes fill it).
        if (!readDone) {
            const int avail = sceMpegRingbufferAvailableSize(&ring_.rb);
            if (avail >= kPutMin || (avail > 0 && reader_.end - reader_.pos < static_cast<uint32_t>(kPutMin) * kPack)) {
                const int put = sceMpegRingbufferPut(&ring_.rb, std::min(avail, kPutMax), avail);
                if (put < 0 || reader_.failures) {
                    fail("read", put < 0 ? put : static_cast<int>(reader_.failures));
                    end = MovieEnd::Failed;
                    break;
                }
                worked |= put > 0;
            }
            readDone = reader_.pos >= reader_.end;
        }
        const bool ringEmpty = sceMpegRingbufferAvailableSize(&ring_.rb) >= ringPackets_;
        // 2. The sound: access units while the PCM ring has room (it holds ~1.1 s), at most 0.75 s ahead. Without
        // sound (no channel, or the decoder failed) they are taken and dropped as they come.
        for (int k = 0; !audioDone_ && k < kAudioPerPass &&
                        (!sound_ || (moviePcmQueued() < kAudioLeadFrames &&
                                     moviePcmRoom() >= static_cast<uint32_t>(pcmBytes_ / 4) * 2u));
             ++k) {
            void* body = nullptr;
            const int r = sceMpegGetAtracAu(&mpeg_, atrac_, &atracAu_.au, &body);
            if (static_cast<uint32_t>(r) == kErrNoData) {
                if (readDone && ringEmpty) audioDone_ = true;
                break;
            }
            if (r < 0) {
                PSP_LOGW(tag::Gfx, "movie_atrac_au r=%08X (the rest without sound)", static_cast<unsigned>(r));
                audioDone_ = true;
                break;
            }
            ++audioAus;
            worked = true;
            if (!sound_) continue;
            sceKernelDcacheWritebackInvalidateRange(pcm_, static_cast<unsigned>(pcmBytes_));
            const int d = sceMpegAtracDecode(&mpeg_, &atracAu_.au, pcm_, atracInit ? 1 : 0);
            if (d < 0) {
                PSP_LOGW(tag::Gfx, "movie_atrac_decode r=%08X (the rest without sound)", static_cast<unsigned>(d));
                moviePcmEnd();  // the system clock goes on from the sound's last time
                sound_ = false;
                continue;
            }
            if (!atracInit) {  // the init call's block is dropped
                if (!audioAnchored_) {
                    audioPts0_ = atracAu_.au.iPts != 0xFFFFFFFFu ? atracAu_.au.iPts : firstTs_;
                    audioAnchored_ = true;
                }
                moviePcmPush(pcm_, static_cast<uint32_t>(pcmBytes_) / 4u);
            }
            atracInit = false;
        }
        if (audioDone_ && sound_ && !tailPadded) {
            // The feeder plays whole 1024-frame blocks: silence completes the last one.
            static const int16_t kSilence[1024 * 2] = {};
            const uint32_t part = moviePcmQueued() % 1024u;
            if (part) moviePcmPush(kSilence, 1024u - part);
            tailPadded = true;
        }
        // 3. The picture: the next access unit decoded into the buffer the GE draws next.
        if (!videoDone && !framePending) {
            void* bufs[1] = {drawTarget()};
            SceInt32 got = 0;
            if (stopped) {
                videoDone = true;
            } else {
                SceInt32 attr = 0;
                const int r = sceMpegGetAvcAu(&mpeg_, avc_, &avcAu_.au, &attr);
                if (static_cast<uint32_t>(r) == kErrNoData) {
                    if (!noDataUs) noDataUs = nowUs();
                    // The end: everything read and demuxed (or nothing more for 2 s after the last read) - the
                    // decoder hands out the picture it still holds.
                    if (readDone && (ringEmpty || nowUs() - noDataUs > 2000000u)) {
                        if (!direct_) sceKernelDcacheWritebackInvalidateRange(ramFrame_, kFrameBytes);
                        const int s = decodedAny_ ? sceMpegAvcDecodeStop(&mpeg_, kStride, bufs, &got) : 0;
                        decodeStopped_ = true;
                        stopped = true;
                        if (s >= 0 && got > 0 && qCount > 0) {
                            framePending = true;
                            framePts = ptsQ[qHead];
                            qHead = (qHead + 1) % 16;
                            --qCount;
                        } else {
                            videoDone = true;
                        }
                    }
                } else if (r < 0) {
                    fail("avc_au", r);
                    end = MovieEnd::Failed;
                    break;
                } else {
                    noDataUs = 0;
                    // Sony's overview: no dirty cache line may sit on the output (a later write-back would overwrite
                    // the picture) - clean the RAM frame first; VRAM is never cached here.
                    if (!direct_) sceKernelDcacheWritebackInvalidateRange(ramFrame_, kFrameBytes);
                    const int d = sceMpegAvcDecode(&mpeg_, &avcAu_.au, kStride, bufs, &got);
                    if (d < 0) {
                        fail("avc_decode", d);
                        end = MovieEnd::Failed;
                        break;
                    }
                    decodedAny_ = true;
                    ++decoded;
                    const uint32_t pts = avcAu_.au.iPts != 0xFFFFFFFFu ? avcAu_.au.iPts : lastAuPts + kFrameTicks;
                    lastAuPts = pts;
                    if (qCount == 16) {  // never with a decoder that holds a picture or two; keep the newest
                        qHead = (qHead + 1) % 16;
                        --qCount;
                    }
                    ptsQ[(qHead + qCount) % 16] = pts;
                    ++qCount;
                    if (got > 0) {
                        framePending = true;
                        framePts = ptsQ[qHead];
                        qHead = (qHead + 1) % 16;
                        --qCount;
                    }
                    worked = true;
                }
            }
        }
        // 4. Show the picture when the clock reaches it; one more than two frames late is dropped (at most 4 in a
        // row: the picture never freezes for long).
        bool flipped = false;
        if (framePending) {
            if (shown == 0 && !audioAnchored_) clockHold_ = framePts;  // no sound yet: the first picture starts the clock
            const int32_t late = static_cast<int32_t>(clock() - framePts);
            if (late > static_cast<int32_t>(2 * kFrameTicks) && dropRun < 4 && !stopped) {
                framePending = false;
                ++dropped;
                ++dropRun;
                worked = true;
            } else if (late >= -static_cast<int32_t>(kFrameTicks / 2)) {
                present(hooks, framePts);
                if (!clockHoldUs_) clockHoldUs_ = nowUs();  // the system clock runs from the first picture
                framePending = false;
                dropRun = 0;
                ++shown;
                flipped = worked = true;
            }
        }
        // 5. The end: the pictures are done and the sound has played out (at most 3 s of draining).
        if (videoDone && !framePending && (audioDone_ || !sound_)) {
            if (!sound_ || moviePcmQueued() == 0) break;
            if (!drainUs) drainUs = nowUs();
            if (nowUs() - drainUs > 3000000u) break;
        }
        // 6. Nothing moves for 5 s (no read, no decode, no sound played): give up.
        const uint32_t played = sound_ ? moviePcmPlayed() : 0u;
        if (worked || played != lastPlayed) {
            lastProgressUs = nowUs();
            lastPlayed = played;
        } else if (nowUs() - lastProgressUs > 5000000u) {
            fail("stall", static_cast<int>(shown));
            end = MovieEnd::Failed;
            break;
        }
        if (nowMs >= nextStatMs) {
            nextStatMs = nowMs + 10000u;
            PSP_LOGI(tag::Gfx, "movie_stat ms=%u shown=%u dropped=%u decoded=%u audio_aus=%u pcm_q=%u underruns=%u "
                     "ring_free=%d reads=%u clock_ms=%u", static_cast<unsigned>((nowUs() - t0) / 1000u),
                     static_cast<unsigned>(shown), static_cast<unsigned>(dropped), static_cast<unsigned>(decoded),
                     static_cast<unsigned>(audioAus), static_cast<unsigned>(moviePcmQueued()),
                     static_cast<unsigned>(moviePcmUnderruns()), static_cast<int>(sceMpegRingbufferAvailableSize(&ring_.rb)),
                     static_cast<unsigned>(reader_.reads), static_cast<unsigned>((clock() - firstTs_) / 90u));
        }
        if (!flipped) sceKernelDelayThread(worked ? 1000 : 4000);  // a flip waited for the vblank already
    }
    PSP_LOGI(tag::Gfx, "movie_end path=%s reason=%s%s%s shown=%u dropped=%u decoded=%u audio_aus=%u underruns=%u reads=%u "
             "read_kb=%u ms=%u", path, movieEndName(end), failStep_[0] ? " step=" : "", failStep_,
             static_cast<unsigned>(shown), static_cast<unsigned>(dropped), static_cast<unsigned>(decoded),
             static_cast<unsigned>(audioAus), static_cast<unsigned>(sound_ ? moviePcmUnderruns() : 0u),
             static_cast<unsigned>(reader_.reads), static_cast<unsigned>((reader_.pos - static_cast<uint32_t>(kPack)) / 1024u),
             static_cast<unsigned>((nowUs() - t0) / 1000u));
    teardown();
    // MovieFrame_OnHide: black, and the frame buffers back to 5551 for whatever draws next.
    frameBegin(0xFF000000u);
    if (rgb565_) {
        sceGuDrawBuffer(GU_PSM_5551, displayDrawBuffer(), kStride);
        rgb565_ = false;
    }
    frameEnd(nullptr, nullptr);
    logMemory("movie_done");
    return end;
}

}  // namespace

const char* movieEndName(MovieEnd e) {
    const unsigned i = static_cast<unsigned>(e);
    return i < sizeof(g_endNames) / sizeof(g_endNames[0]) ? g_endNames[i] : "?";
}

MovieEnd moviePlay(const char* absPath, int volume, const MovieHooks& hooks) {
    Movie m;
    return m.play(absPath, volume, hooks);
}

} // namespace wowee::platform::psp
