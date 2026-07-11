#!/usr/bin/env bash
#
# hd_music_pipeline.sh - HD music remaster A/B pipeline (Phase S1+)
#
# Produces and manages alternative "HD" renders of Tyrian's LDS/OPL2 music
# tracks so they can be auditioned against the classic OPL render before
# installing one into the live data dir as hdmusic_NN.ogg.
#
# Workdir: ./hdmusic_work (created on demand, repo-root-relative; gitignored)
#   hdmusic_work/bin/            built tools (render_music, lds_to_midi)
#   hdmusic_work/classic/        render_music output (wav/.loop) per song
#   hdmusic_work/midi/           lds_to_midi output (.mid/.loopinfo/.patches)
#   hdmusic_work/variants/       encoded candidate OGGs, hdmusic_NN.<name>.ogg
#   hdmusic_work/backup/         one-time backup of the shipped ogg per song
#   hdmusic_work/sf/             (you provide) soundfonts for the `gm` command
#
# Commands:
#   pipeline.sh build
#       Compile tools/render_music.c and tools/lds_to_midi.c into
#       hdmusic_work/bin/.
#
#   pipeline.sh classic <NN>
#       Render song NN (1-based) with the classic OPL synth via render_music,
#       then encode to hdmusic_work/variants/hdmusic_NN.classic.ogg with
#       LOOPSTART/LOOPLENGTH Vorbis comments.
#
#   pipeline.sh midi <NN>
#       Run lds_to_midi for song NN into hdmusic_work/midi/; print the
#       .patches instrument report and loop info.
#
#   pipeline.sh gm <NN> <soundfont.sf2> [variantname]
#       lds_to_midi (if needed) -> fluidsynth render -> trim to TOTALSAMPLES
#       (from .loopinfo) -> encode ogg with loop tags ->
#       hdmusic_work/variants/hdmusic_NN.<variantname|sf2-basename>.ogg.
#
#   pipeline.sh import <NN> <file.wav|ogg> <variantname>
#       Fit an externally produced render into the variants directory:
#       resample to 44100 Hz stereo if needed; if a matching .loopinfo
#       exists and the file's length is within 1% of TOTALSAMPLES, trim and
#       tag loop comments; otherwise encode untrimmed with a warning.
#
#   pipeline.sh install <NN> <variantname>
#       Back up the shipped ogg (once) then copy the variant over
#       $DATA_DIR/hdmusic_NN.ogg.
#
#   pipeline.sh restore <NN>
#       Restore the backed-up shipped ogg for song NN.
#
#   pipeline.sh list <NN>
#       List variants for song NN with duration/size/loop tags.
#
# Env:
#   DATA_DIR   Tyrian data dir (default: /Users/felixhuang/source/opentyrian/tyrian21)
#
# Only `install` and `restore` write into DATA_DIR.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKDIR="$REPO_ROOT/hdmusic_work"
BINDIR="$WORKDIR/bin"
CLASSICDIR="$WORKDIR/classic"
MIDIDIR="$WORKDIR/midi"
VARIANTDIR="$WORKDIR/variants"
BACKUPDIR="$WORKDIR/backup"
SFDIR="$WORKDIR/sf"

DATA_DIR="${DATA_DIR:-/Users/felixhuang/source/opentyrian/tyrian21}"

RENDER_MUSIC="$BINDIR/render_music"
LDS_TO_MIDI="$BINDIR/lds_to_midi"

usage() {
	sed -n '2,60p' "${BASH_SOURCE[0]}" | sed 's/^#//; s/^ //'
	exit 1
}

need_cmd() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "error: '$1' not found on PATH" >&2
		if [ "$1" = "fluidsynth" ]; then
			echo "hint: brew install fluid-synth" >&2
		fi
		exit 1
	fi
}

pad2() {
	printf '%02d' "$1"
}

cmd_build() {
	need_cmd cc
	need_cmd pkg-config
	mkdir -p "$BINDIR"

	echo "building render_music..."
	cc -O2 -std=iso9899:1999 $(pkg-config --cflags sdl2) \
		"$REPO_ROOT/tools/render_music.c" "$REPO_ROOT/src/lds_play.c" "$REPO_ROOT/src/opl.c" "$REPO_ROOT/src/musmast.c" \
		$(pkg-config --libs sdl2) -lm -o "$RENDER_MUSIC"

	echo "building lds_to_midi..."
	cc -O2 -std=iso9899:1999 $(pkg-config --cflags sdl2) \
		"$REPO_ROOT/tools/lds_to_midi.c" "$REPO_ROOT/src/lds_play.c" "$REPO_ROOT/src/musmast.c" \
		$(pkg-config --libs sdl2) -lm -o "$LDS_TO_MIDI"

	echo "build ok: $RENDER_MUSIC, $LDS_TO_MIDI"
}

require_built() {
	if [ ! -x "$RENDER_MUSIC" ] || [ ! -x "$LDS_TO_MIDI" ]; then
		echo "tools not built yet; running 'build'..." >&2
		cmd_build
	fi
}

cmd_classic() {
	local nn="${1:?usage: pipeline.sh classic <NN>}"
	local nn2; nn2=$(pad2 "$nn")
	require_built
	need_cmd ffmpeg

	mkdir -p "$CLASSICDIR" "$VARIANTDIR"

	SDL_AUDIODRIVER=dummy "$RENDER_MUSIC" "$DATA_DIR" "$CLASSICDIR" "$nn"

	local wav="$CLASSICDIR/hdmusic_${nn2}.wav"
	local loop="$CLASSICDIR/hdmusic_${nn2}.loop"
	local out="$VARIANTDIR/hdmusic_${nn2}.classic.ogg"

	if [ ! -f "$wav" ]; then
		echo "error: expected $wav, render_music didn't produce it" >&2
		exit 1
	fi

	local meta_args=()
	if [ -f "$loop" ]; then
		local loopstart looplength
		loopstart=$(awk '/^LOOPSTART/ {print $2}' "$loop")
		looplength=$(awk '/^LOOPLENGTH/ {print $2}' "$loop")
		meta_args=(-metadata "LOOPSTART=$loopstart" -metadata "LOOPLENGTH=$looplength")
	fi

	ffmpeg -y -loglevel error -i "$wav" -c:a libvorbis -qscale:a 6 "${meta_args[@]}" "$out"
	echo "wrote $out"
}

cmd_midi() {
	local nn="${1:?usage: pipeline.sh midi <NN>}"
	local nn2; nn2=$(pad2 "$nn")
	require_built
	mkdir -p "$MIDIDIR"

	"$LDS_TO_MIDI" "$DATA_DIR" "$MIDIDIR" "$nn"

	local patches="$MIDIDIR/hdmusic_${nn2}.patches"
	local loopinfo="$MIDIDIR/hdmusic_${nn2}.loopinfo"

	if [ -f "$patches" ]; then
		echo "--- patches report: $patches ---"
		cat "$patches"
	fi
	if [ -f "$loopinfo" ]; then
		echo "--- loop info: $loopinfo ---"
		cat "$loopinfo"
	else
		echo "(no loop detected for song $nn)"
	fi
}

ensure_midi() {
	local nn="$1" nn2="$2"
	if [ ! -f "$MIDIDIR/hdmusic_${nn2}.mid" ]; then
		require_built
		mkdir -p "$MIDIDIR"
		"$LDS_TO_MIDI" "$DATA_DIR" "$MIDIDIR" "$nn"
	fi
}

cmd_gm() {
	local nn="${1:?usage: pipeline.sh gm <NN> <soundfont.sf2> [variantname]}"
	local sf2="${2:?usage: pipeline.sh gm <NN> <soundfont.sf2> [variantname]}"
	local variant="${3:-$(basename "$sf2" .sf2)}"
	local nn2; nn2=$(pad2 "$nn")

	need_cmd ffmpeg
	need_cmd fluidsynth

	if [ ! -f "$sf2" ]; then
		echo "error: soundfont not found: $sf2" >&2
		exit 1
	fi

	ensure_midi "$nn" "$nn2"

	local mid="$MIDIDIR/hdmusic_${nn2}.mid"
	local loopinfo="$MIDIDIR/hdmusic_${nn2}.loopinfo"

	if [ ! -f "$mid" ]; then
		echo "error: expected $mid" >&2
		exit 1
	fi

	mkdir -p "$VARIANTDIR"
	local rawwav="$VARIANTDIR/.tmp_hdmusic_${nn2}_${variant}_raw.wav"
	local trimwav="$VARIANTDIR/.tmp_hdmusic_${nn2}_${variant}_trim.wav"
	local out="$VARIANTDIR/hdmusic_${nn2}.${variant}.ogg"

	echo "rendering with fluidsynth ($sf2, gain ${FLUID_GAIN:-0.6})..."
	fluidsynth -ni -g "${FLUID_GAIN:-0.6}" -r 44100 -F "$rawwav" "$sf2" "$mid"

	local meta_args=()
	if [ -f "$loopinfo" ]; then
		local totalsamples loopstart looplength
		totalsamples=$(awk '/^TOTALSAMPLES/ {print $2}' "$loopinfo")
		loopstart=$(awk '/^LOOPSTART / {print $2}' "$loopinfo")
		looplength=$(awk '/^LOOPLENGTH / {print $2}' "$loopinfo")

		ffmpeg -y -loglevel error -i "$rawwav" -af "atrim=end_sample=${totalsamples}" "$trimwav"
		meta_args=(-metadata "LOOPSTART=$loopstart" -metadata "LOOPLENGTH=$looplength")
	else
		echo "(no loop info for song $nn; encoding full render untrimmed)"
		trimwav="$rawwav"
	fi

	ffmpeg -y -loglevel error -i "$trimwav" -c:a libvorbis -qscale:a 6 "${meta_args[@]}" "$out"

	rm -f "$rawwav" "$trimwav"
	echo "wrote $out"
}

cmd_import() {
	local nn="${1:?usage: pipeline.sh import <NN> <file.wav|ogg> <variantname>}"
	local infile="${2:?usage: pipeline.sh import <NN> <file.wav|ogg> <variantname>}"
	local variant="${3:?usage: pipeline.sh import <NN> <file.wav|ogg> <variantname>}"
	local nn2; nn2=$(pad2 "$nn")

	need_cmd ffmpeg
	need_cmd ffprobe

	if [ ! -f "$infile" ]; then
		echo "error: file not found: $infile" >&2
		exit 1
	fi

	mkdir -p "$VARIANTDIR"

	local rate channels
	rate=$(ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate -of csv=p=0 "$infile")
	channels=$(ffprobe -v error -select_streams a:0 -show_entries stream=channels -of csv=p=0 "$infile")

	local work="$VARIANTDIR/.tmp_import_${nn2}_${variant}.wav"
	if [ "$rate" != "44100" ] || [ "$channels" != "2" ]; then
		echo "resampling $infile (${rate}Hz/${channels}ch) -> 44100Hz/2ch"
		ffmpeg -y -loglevel error -i "$infile" -ar 44100 -ac 2 "$work"
	else
		ffmpeg -y -loglevel error -i "$infile" -ar 44100 -ac 2 "$work"
	fi

	local loopinfo="$MIDIDIR/hdmusic_${nn2}.loopinfo"
	local out="$VARIANTDIR/hdmusic_${nn2}.${variant}.ogg"

	if [ -f "$loopinfo" ]; then
		local totalsamples loopstart looplength
		totalsamples=$(awk '/^TOTALSAMPLES/ {print $2}' "$loopinfo")
		loopstart=$(awk '/^LOOPSTART / {print $2}' "$loopinfo")
		looplength=$(awk '/^LOOPLENGTH / {print $2}' "$loopinfo")

		local samples
		samples=$(ffprobe -v error -select_streams a:0 -show_entries stream=duration -of csv=p=0 "$work" | awk '{printf "%d", $1*44100}')

		local diffpct
		diffpct=$(awk -v a="$samples" -v b="$totalsamples" 'BEGIN { d=a-b; if(d<0) d=-d; printf "%.4f", (d/b)*100 }')
		local within
		within=$(awk -v d="$diffpct" 'BEGIN { print (d <= 1.0) ? 1 : 0 }')

		if [ "$within" = "1" ]; then
			local trimwav="$VARIANTDIR/.tmp_import_${nn2}_${variant}_trim.wav"
			ffmpeg -y -loglevel error -i "$work" -af "atrim=end_sample=${totalsamples}" "$trimwav"
			ffmpeg -y -loglevel error -i "$trimwav" -c:a libvorbis -qscale:a 6 \
				-metadata "LOOPSTART=$loopstart" -metadata "LOOPLENGTH=$looplength" "$out"
			rm -f "$trimwav"
			echo "wrote $out (trimmed + loop-tagged, within ${diffpct}% of expected length)"
		else
			echo "warning: import length differs from expected TOTALSAMPLES by ${diffpct}% (>1%);" >&2
			echo "         encoding WITHOUT loop tags -- it will play through then loop from 0" >&2
			echo "         as the engine dictates." >&2
			ffmpeg -y -loglevel error -i "$work" -c:a libvorbis -qscale:a 6 "$out"
			echo "wrote $out (no loop tags)"
		fi
	else
		echo "warning: no $loopinfo found; encoding WITHOUT loop tags -- it will play" >&2
		echo "         through then loop from 0 as the engine dictates." >&2
		ffmpeg -y -loglevel error -i "$work" -c:a libvorbis -qscale:a 6 "$out"
		echo "wrote $out (no loop tags)"
	fi

	rm -f "$work"
}

cmd_install() {
	local nn="${1:?usage: pipeline.sh install <NN> <variantname>}"
	local variant="${2:?usage: pipeline.sh install <NN> <variantname>}"
	local nn2; nn2=$(pad2 "$nn")

	local variantfile="$VARIANTDIR/hdmusic_${nn2}.${variant}.ogg"
	local shipped="$DATA_DIR/hdmusic_${nn2}.ogg"
	local backup="$BACKUPDIR/hdmusic_${nn2}.ogg"

	if [ ! -f "$variantfile" ]; then
		echo "error: variant not found: $variantfile" >&2
		exit 1
	fi

	mkdir -p "$BACKUPDIR"

	if [ ! -f "$backup" ]; then
		if [ -f "$shipped" ]; then
			cp "$shipped" "$backup"
		else
			echo "error: no shipped ogg at $shipped to back up; refusing to install" >&2
			exit 1
		fi
	fi

	if [ ! -f "$backup" ]; then
		echo "error: backup step failed, refusing to install" >&2
		exit 1
	fi

	cp "$variantfile" "$shipped"
	echo "installed $variantfile -> $shipped (backup at $backup)"
}

cmd_restore() {
	local nn="${1:?usage: pipeline.sh restore <NN>}"
	local nn2; nn2=$(pad2 "$nn")

	local backup="$BACKUPDIR/hdmusic_${nn2}.ogg"
	local shipped="$DATA_DIR/hdmusic_${nn2}.ogg"

	if [ ! -f "$backup" ]; then
		echo "error: no backup found at $backup" >&2
		exit 1
	fi

	cp "$backup" "$shipped"
	echo "restored $backup -> $shipped"
}

cmd_list() {
	local nn="${1:?usage: pipeline.sh list <NN>}"
	local nn2; nn2=$(pad2 "$nn")
	need_cmd ffprobe

	shopt -s nullglob
	local files=("$VARIANTDIR"/hdmusic_${nn2}.*.ogg)
	shopt -u nullglob

	if [ ${#files[@]} -eq 0 ]; then
		echo "no variants found for song $nn in $VARIANTDIR"
		return 0
	fi

	for f in "${files[@]}"; do
		local dur size loopstart looplength
		dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$f")
		size=$(du -h "$f" | awk '{print $1}')
		loopstart=$(ffprobe -v error -select_streams a:0 -show_entries stream_tags=LOOPSTART -of csv=p=0 "$f" 2>/dev/null || true)
		looplength=$(ffprobe -v error -select_streams a:0 -show_entries stream_tags=LOOPLENGTH -of csv=p=0 "$f" 2>/dev/null || true)
		printf '%-60s %8ss  %6s  LOOPSTART=%s LOOPLENGTH=%s\n' "$(basename "$f")" "$dur" "$size" "${loopstart:-none}" "${looplength:-none}"
	done
}

main() {
	if [ $# -lt 1 ]; then
		usage
	fi

	local sub="$1"; shift

	case "$sub" in
		build)   cmd_build "$@" ;;
		classic) cmd_classic "$@" ;;
		midi)    cmd_midi "$@" ;;
		gm)      cmd_gm "$@" ;;
		import)  cmd_import "$@" ;;
		install) cmd_install "$@" ;;
		restore) cmd_restore "$@" ;;
		list)    cmd_list "$@" ;;
		-h|--help|help) usage ;;
		*)
			echo "error: unknown command '$sub'" >&2
			usage
			;;
	esac
}

main "$@"
