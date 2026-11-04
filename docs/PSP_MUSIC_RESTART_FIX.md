# Zone music restart and metadata correction

The user's actual Kiln audio.log reported "authored music: 0 area rules" while invoking --no-original. That option excludes external file: soundtrack paths; it must not suppress the client ZoneMusic/AreaTable day/night and silence metadata. The converter now always builds the authored client rules.

A submitted or pending MP3 request previously counted as successful playback. When an open/decode failed, the world scheduler entered the normal multi-minute post-track silence. AudioStats now counts tracks which produced a decoded frame; a start failure instead retries after two seconds. Completed tracks still use the authored silence intervals.

The PSP also needs the complete converted data/music directory. audio.pak contains references, not MP3 data. Missing files cannot be repaired by the scheduler. Preserve session.log after reproducing if the new runtime and matching music files remain silent; music_open_failed and music_start_retry distinguish missing files/descriptor errors from normal music_silence.

Rules with a nonzero SoundEntries ID but no available files can fall back to a usable legacy zone playlist; explicit authored silence (soundId zero) remains silent. A corrected real-client conversion produced 793 rules and 6789 references instead of zero rules. It still reported 46 missing source references, so file presence is checked separately from successful metadata generation.
