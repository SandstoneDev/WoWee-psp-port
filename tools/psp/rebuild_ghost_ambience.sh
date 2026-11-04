#!/usr/bin/env bash
set -euo pipefail
if [ "$#" -ne 2 ]; then
  echo 'Usage: rebuild_ghost_ambience.sh /path/to/extracted /path/to/game/data' >&2
  exit 2
fi
command -v ffmpeg >/dev/null || { echo 'ffmpeg is required to transcode the original GhostState.wav.' >&2; exit 1; }
source_audio="$1/sound/ambience/ghoststate.wav"
target_audio="$2/music/ambience/ghoststate.mp3"
[ -f "$source_audio" ] || { echo "Missing: $source_audio" >&2; exit 1; }
mkdir -p "$(dirname "$target_audio")"
ffmpeg -nostdin -y -i "$source_audio" -vn -ar 44100 -ac 2 -c:a libmp3lame -b:a 128k "$target_audio"
