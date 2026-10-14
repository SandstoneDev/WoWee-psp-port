// PSP audio output (M12): streamed music through the hardware MP3 decoder and
// a small software mixer for in-memory sound effects.
//
// Rules this follows (psp-developer skill, references/audio.md - each one
// cost a sibling port a session of silence or frame spikes):
//  - no SRC channel (re-reserving it after a release fails on hardware):
//    plain sceAudio channels at 44.1 kHz, own 16.16 resampling;
//  - decoder/feeder split: a LOW priority thread reads the card and decodes
//    into a PCM ring (~1 s), a HIGH priority thread only feeds the DAC from
//    RAM - a card stall cannot stutter the sound, the sound cannot stall a
//    frame;
//  - threads and channels are created once per session; a new track is a
//    request with a generation number, never a new thread;
//  - files are opened by absolute path (worker threads have no cwd).
// MP3 decode runs in the firmware's sceMp3 (Media Engine), not on the CPU.
#pragma once

#include <cstdint>

namespace wowee::platform::psp {

struct AudioStats {
    uint32_t underruns = 0;     // feeder blocks with no PCM ready
    uint32_t decodeErrors = 0;
    uint32_t ringFill = 0;      // stereo frames buffered
    uint32_t sfxVoices = 0;     // voices playing
};

// Loads the AV modules, reserves the channels, starts the threads.
bool audioInit();
void audioShutdown();
const AudioStats& audioStats();

// Music (one stream). `absPath` = an MP3 on the Memory Stick. The previous
// track fades out over fadeOutMs before the new one starts at gain 0 and
// fades in over fadeInMs. loop = restart at the end.
void musicPlay(const char* absPath, bool loop, uint32_t fadeOutMs = 1000, uint32_t fadeInMs = 1000);
void musicStop(uint32_t fadeOutMs = 1000);
void musicSetVolume(float v);  // 0..1 (category volume, multiplied with the fades)
bool musicActive();            // a track is decoding or still audible

// Sound effects: PCM already in RAM (the caller keeps it alive while it
// plays). Returns a voice id (>= 0) or -1 when every voice is busy with a
// louder sound. gain 0..1, pan -1 (left) .. 1 (right).
int sfxPlay(const int16_t* pcm, uint32_t frames, int channels, uint32_t rate, float gain, float pan, bool loop);
void sfxStop(int voice);
void sfxSetGain(int voice, float gain, float pan);
void sfxSetVolume(float v);    // category volume
bool sfxPlaying(int voice);
// Sounds - stops every voice (the caller frees the PCM a block later).
void sfxStopAll();

// Cinematics: a movie's sound (psp_movie.cpp: ATRAC3plus decoded by sceMpeg, 44.1 kHz stereo
// s16) through the music channel. moviePcmBegin stops the music at once (the original stops the glue music when a
// movie starts) and hands the music's PCM ring to the movie; the feeder plays it at `gain` (no category volume, no
// fades). moviePcmPush copies frames in (false when the ring lacks room); moviePcmPlayed counts the frames the
// feeder gave the DAC since begin - the movie's clock (it stops while the ring runs dry). moviePcmEnd empties the
// ring and gives it back: the music decoder (which waits meanwhile) acts on the next musicPlay.
bool moviePcmBegin(float gain);
uint32_t moviePcmRoom();       // stereo frames free in the ring
uint32_t moviePcmQueued();     // stereo frames waiting in the ring
bool moviePcmPush(const int16_t* pcm, uint32_t frames);
uint32_t moviePcmPlayed();     // 44.1 kHz frames (wraps after 27 hours)
uint32_t moviePcmUnderruns();  // feeder blocks the movie had nothing ready for
void moviePcmEnd();

} // namespace wowee::platform::psp
