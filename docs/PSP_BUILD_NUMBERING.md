# PSP build numbering

The polish rebuild launcher now uses the normal build_psp.sh path, which
increments psp/BUILD_NUMBER and embeds that value through WOWEE_PSP_BUILD in the
EBOOT. The previous launcher forced --no-bump and reused build 118 across many
rebuilds. Their exact count cannot be recovered from the overwritten BUILD file.
The existing baseline remains 118; the next rebuild attempt is 119, then 120.

After successful output verification, the build writes psp/bin/BUILD and appends
the build ID, UTC timestamp and EBOOT MD5 to psp/bin/build-history.tsv. Build IDs
advance when compilation starts, so failed attempts can leave gaps; the history
contains successful builds only. Direct --no-bump remains available explicitly
for reproducing a particular build, but the normal launcher never supplies it.

The ordinary command is unchanged:

```bash
bash tools/psp/build_psp.sh
```
