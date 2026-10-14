// Read-only pack file ("WPAK") produced by tools/psp/build_ui_pak.py.
//
// One file, one descriptor, a TOC read once: on the Memory Stick an open
// costs 17-34 ms while bytes are nearly free, so assets ship in packs.
//
// Layout (little-endian):
//   header  : "WPAK" u32 version(1) u32 count u32 tocOffset
//   toc     : count x { char name[48]; u32 offset; u32 size; u32 kind; u32 reserved }
//   entries : each 64-byte aligned
#pragma once

#include <cstddef>
#include <cstdint>

#include "platform/psp/psp_fd_budget.hpp"  // the descriptor budget

namespace wowee::platform::psp {

// The TOC record on the card (unchanged since v1).
struct PakDiskEntry {
    char name[48];
    uint32_t offset;
    uint32_t size;
    uint32_t kind;      // 1 = texture (WTEX), 2 = font (WFNT), 3 = raw
    uint32_t reserved;
};
static_assert(sizeof(PakDiskEntry) == 64, "PakDiskEntry is a 64-byte TOC record");

// For whole-world data (heap headroom 2.6 MB): the TOC in memory - 16 bytes a record, the name in one
// pool of exact lengths (was the 64-byte disk record: ~1 MB for the paks open in the world, characters.pak
// alone 561 KB). The disk format is the same: no pak has to be rebaked.
struct PakEntry {
    const char* name;   // into the Pak's name pool, NUL-terminated (at most 47 chars)
    uint32_t offset;
    uint32_t size;
    uint32_t kind;      // 1 = texture (WTEX), 2 = font (WFNT), 3 = raw
};

class Pak {
public:
    Pak() = default;
    ~Pak();
    Pak(const Pak&) = delete;
    Pak& operator=(const Pak&) = delete;

    bool open(const char* absPath);
    void close();
    bool isOpen() const { return count_ > 0; }  // also while the descriptor is given back
    // The descriptor, for a reader thread sharing it through pakRead (because of
    // the card driver's open-file limit). -1 while given back (releaseFd).
    int fd() const { return fd_; }
    // On hardware the card driver ran out of open files (0x80020320 - a window lost its art): a pak
    // read only now and then gives its descriptor back between reads. The TOC stays; the next load() reopens the
    // file (one open, 17-34 ms on the Memory Stick). Only for a pak used by one thread through load().
    void releaseFd();
    // releaseFd() once load() has not run for idleMs.
    void releaseIfIdle(uint32_t nowMs, uint32_t idleMs);

    // Binary search over a name index built at open() (was a linear
    // strcmp scan: ~0.3 ms per find over the 1.8k-entry doodads TOC on the
    // PSP - every model, texture and collision mesh of a streamed tile finds
    // its entry). Same result: the first entry with that name in TOC order.
    const PakEntry* find(const char* name) const;
    uint32_t count() const { return count_; }
    const PakEntry* entry(uint32_t i) const { return i < count_ ? &toc_[i] : nullptr; }

    // Reads the entry into a 64-byte aligned heap block (dcache written back,
    // so the GE can read it at once). Caller frees with pakFree().
    void* load(const PakEntry* e);
    // `size` bytes at `offset` inside the entry into dst - a slice of a big table
    // without a heap block for all of it (transports.pak's tpaths: one path of ~80). The descriptor is opened again
    // when given back (releaseFd), as load() does. False when the slice is outside the entry or the read fails.
    bool readPart(const PakEntry* e, uint32_t offset, void* dst, uint32_t size);

    // Streaming. Identifies the file (FNV-1a of the path given to
    // open): two Pak objects on one file share it, so blobs read ahead for it
    // (pakPrefetchPut) are handed out by whichever of them loads the entry.
    uint32_t fileId() const { return fileId_; }

    // The descriptor budget (psp_fd_budget.hpp): a rare pak - read now and then by the main thread only,
    // its descriptor never handed to another thread - may be asked for its descriptor by pakFdRelieve (an open
    // failed for want of one); releaseIfIdle's idle time shrinks when descriptors are short (for every caller).
    void setRare(bool on);
    bool rare() const { return rare_; }
    // Memory diet: load() takes its block from the world arena first (pp::worldArenaAlloc - the
    // doodads pak: model / texture / collision blobs live as long as their tile, never pin newlib). Kept until close().
    void setWorldArena(bool on) { worldArena_ = on; }
    // The descriptor again after releaseFd (true when open); for a caller that hands fd() to a reader thread.
    bool ensureFd();

private:
    bool rare_ = false;          // the descriptor budget
    bool worldArena_ = false;    // setWorldArena
    static uint32_t fdLastUse(void* self);  // FdRegistry hooks
    static void fdRelease(void* self);
    bool reopenFd();             // after releaseFd
    char path_[96] = {};         // the file, for reopenFd
    uint32_t lastUseMs_ = 0;     // the last load(), for releaseIfIdle
    int fd_ = -1;
    uint32_t count_ = 0;
    PakEntry* toc_ = nullptr;    // in block_, the name pool after it
    void* block_ = nullptr;
    uint16_t* index_ = nullptr;  // TOC indices sorted by name (stable)
    uint32_t fileId_ = 0;        // pakFileId(path)
};

void pakFree(void* block);

// Tile streaming (src/ui/psp/ui_world_streaming.cpp). A loader
// thread reads entries ahead into pak-style blobs (64-byte aligned, D-cache
// written back); the main thread parks them here and Pak::load hands a
// parked blob out instead of reading the card, so the unchanged load paths
// (TerrainRenderer/ModelRenderer::loadTile, CreatureRenderer spawns) install
// a tile without touching the Memory Stick. Main thread only.
uint32_t pakFileId(const char* absPath);
// Thread-safe read of any pak descriptor (one lock, 64 KB chunks).
bool pakRead(int fd, uint32_t offset, void* dst, uint32_t size);
// How many more files can be opened now (opens `absPath` up to `max` times,
// closes them): the card driver's headroom, logged at world entry.
int pakProbeFreeFiles(const char* absPath, int max);
void pakPrefetchPut(uint32_t fileId, uint32_t offset, void* blob);
// The parked blob for that entry (ownership to the caller), or nullptr.
void* pakPrefetchTake(uint32_t fileId, uint32_t offset);
// Peek without taking (nullptr when not parked).
const void* pakPrefetchPeek(uint32_t fileId, uint32_t offset);
// Frees every blob parked for the file; returns how many.
uint32_t pakPrefetchDrop(uint32_t fileId);
uint32_t pakPrefetchCount();
// Entries Pak::load has read from the card (not parked) since boot: the
// streaming log counts the reads an install still did itself (misses).
uint32_t pakReadCount();
// Pak descriptors open now (the fd_probe line; when an open fails for want of a descriptor, the log lists
// them: pak_open_list).
int pakOpenFds();
// The open-descriptor list for other pak readers (PakIndex) - pak_open_list names them too.
void pakNoteOpen(int fd, const char* path);
void pakNoteClose(int fd);
void pakLogOpenFail(const char* path, int err);
// Called after every pakRead (any thread) while set - the world load's CMSG_KEEP_ALIVE inside its long
// steps (LoginScreen::enterWorldScreen). nullptr clears it.
using PakReadTick = void (*)(void* user);
void pakSetReadTick(PakReadTick fn, void* user);

// ---- the descriptor budget (psp_fd_budget.hpp) ----
// Every file the game opens notes its descriptor (pakNoteOpen / pakNoteClose, not only the paks); the capacity is
// what fd_probe found free at world entry + what was open then. Rare paks (Pak::setRare, PakIndex::setRare) give
// their descriptor back sooner when one or none is left (releaseIfIdle: 1 s), at once when an open failed for want of
// one (0x80020320): the main thread's open retries after pakFdRelieve, another thread's sets the pressure that
// pakFdService (the main thread, every frame) answers.
void pakSetMainThread();
void pakFdSetCapacity(int cap);
int pakFdCapacity();
int pakFdFree();         // -1 before the capacity is known
bool pakFdTight();       // one descriptor or none left
uint32_t pakFdIdleMs(uint32_t idleMs);  // a rare pak's idle rule now (idleMs, or shorter)
void pakFdSetOwner(int fd, void* owner, FdLastUseFn lastUse, FdReleaseFn release);
int pakFdRelieve(int want);  // main thread: up to `want` idle rare paks give their descriptor back; how many did
void pakFdService();         // main thread, every frame
int pakFdList(char* buf, size_t cap);  // "name,name*,..." ('*' rare); returns how many are open
void pakFdStats(uint32_t& mfiles, uint32_t& relieved, int& rare);
int pakOpenRetry(const char* absPath);  // sceIoOpen read-only, retried once after pakFdRelieve on MFILE
// ---- the descriptor budget (end) ----

} // namespace wowee::platform::psp
