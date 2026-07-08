#!/bin/sh
# Encode render_music's WAV output to OGG Vorbis (hdmusic_NN.ogg), embedding
# LOOPSTART/LOOPLENGTH Vorbis comments (sample counts) from the .loop
# sidecar files render_music writes alongside each WAV.
#
# Usage: tools/encode_music.sh <wav_dir> <out_data_dir> [quality]
#
# <wav_dir>      directory containing hdmusic_NN.wav + hdmusic_NN.loop
#                (output of tools/render_music, see tools/MUSIC_REMASTER.md)
# <out_data_dir> directory to write hdmusic_NN.ogg into (a Tyrian data dir)
# [quality]      libvorbis -qscale:a value, default 6 (~192 kbps, high quality)
#
# Requires ffmpeg (with libvorbis) on PATH.

set -e

WAV_DIR="$1"
OUT_DIR="$2"
QUALITY="${3:-6}"

if [ -z "$WAV_DIR" ] || [ -z "$OUT_DIR" ]; then
	echo "usage: $0 <wav_dir> <out_data_dir> [quality]" >&2
	exit 1
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
	echo "error: ffmpeg not found on PATH" >&2
	exit 1
fi

mkdir -p "$OUT_DIR"

for wav in "$WAV_DIR"/hdmusic_*.wav; do
	[ -e "$wav" ] || continue

	base=$(basename "$wav" .wav)
	loopfile="$WAV_DIR/$base.loop"
	out="$OUT_DIR/$base.ogg"

	META_ARGS=""
	if [ -f "$loopfile" ]; then
		LOOPSTART=$(awk '/^LOOPSTART/ {print $2}' "$loopfile")
		LOOPLENGTH=$(awk '/^LOOPLENGTH/ {print $2}' "$loopfile")
		META_ARGS="-metadata LOOPSTART=$LOOPSTART -metadata LOOPLENGTH=$LOOPLENGTH"
	fi

	echo "encoding $base -> $out"
	# shellcheck disable=SC2086
	ffmpeg -y -loglevel error -i "$wav" -c:a libvorbis -qscale:a "$QUALITY" $META_ARGS "$out"
done

echo "done."
