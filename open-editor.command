#!/bin/bash
#
# open-editor.command -- double-click in Finder to launch the OpenTyrian
# level editor. macOS runs a *.command file in Terminal.app on double-click
# (a plain extensionless script would not), so this file carries the
# .command suffix and the executable bit.
#
# It cd's to its own directory so the relative binary/data paths resolve no
# matter what working directory Finder hands it, builds the game if needed,
# then boots straight into the editor, which now shows its own in-app
# episode picker (1-4) followed by that episode's level-select screen.
# See internal/plan/LEVEL_EDITOR_PLAN.md for the editor.

# Resolve this script's own directory (the repo root) and work from there.
cd "$(dirname "$0")" || exit 1

BINARY=./opentyrian
DATA_DIR=./tyrian21

echo "=== OpenTyrian level editor ==="
echo

# Build on demand if the binary isn't there yet.
if [ ! -x "$BINARY" ]; then
	echo "Binary not found -- building with 'make'..."
	if ! make; then
		echo
		echo "Build failed. Fix the errors above, then try again."
		read -r -p "Press Return to close..." _
		exit 1
	fi
	echo
fi

# The game ships with no data; the editor needs the Tyrian 2.1 files.
if [ ! -d "$DATA_DIR" ]; then
	echo "Data directory '$DATA_DIR' not found."
	echo "Place the Tyrian 2.1 data files there (https://camanis.net/tyrian/tyrian21.zip)."
	read -r -p "Press Return to close..." _
	exit 1
fi

echo "Launching editor..."
echo "(You'll pick an episode, then a level, inside the editor itself.)"
echo "(In-editor: T tile sidebar, [ ] pick tile, Enter place, U/R undo/redo,"
echo " E events, S save, X export, Esc back. F1 toggles the help lines.)"
echo "(Mouse: click/drag selects the cell, right-click picks; click the left strip to scroll, wheel scrolls. Place with Enter/Space.)"
echo

"$BINARY" --data "$DATA_DIR" --edit
