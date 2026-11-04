# Icon and spell file pressure recovery

The 47c00e3e session records 0x80020320 (MFILE), ten registered files at the
measured capacity of ten, and failures opening item icons, UI, text, spell
effects and the Undercity pack. These failures do not prove pack corruption.

Item icons and spell effects now release their descriptor immediately after
a completed read when the shared file budget is tight. Cached textures and
display mappings remain resident. With spare slots, the existing idle timeout
still applies. Spell read failures also close the failed descriptor.

Temporary spell texture allocation/open/read failures retry after one second
instead of marking the texture permanently broken. Invalid records and parsed
texture corruption remain terminal. Effect I/O failures no longer emit the
misleading instruction to rebake spellfx.pak; that message is reserved for
an effect blob which was read successfully but fails validation.

Host regressions passed: test_psp_item_icons.cpp and
test_psp_spell_file_pressure.py. Covered pressure-time handle release, cached
visibility, bounded retries, recovery, corrupt assets and spare-slot behavior.
This reduces two descriptor holders; it does not guarantee elimination of
all MFILE events from other simultaneously active readers. Hardware needs a
new session log after rebuilding EBOOT.PBP. No asset rebake required.
