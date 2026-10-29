#!/usr/bin/env bash
# Headless smoke test: starts a real OBS under Xvfb with a synthetic, noisy video containing
# changing cue labels, applies the filter and prints the detected cues from the OBS log.
# Needs: obs-studio xvfb ffmpeg python3-pil python3-numpy fonts-dejavu-core + the built plugin.
set -euo pipefail
SO="${1:-build/obs-cue-chapters.so}"
python3 "$(dirname "$0")/genvid.py"                       # -> /tmp/cue_test.mp4
D=~/.config/obs-studio
mkdir -p "$D/plugins/obs-cue-chapters/bin/64bit" "$D/basic/scenes" "$D/basic/profiles/Untitled" /tmp/rec
cp "$SO" "$D/plugins/obs-cue-chapters/bin/64bit/"
cp "$(dirname "$0")/Test.json.template" "$D/basic/scenes/Test.json"
printf '[General]\nFirstRun=true\n[Basic]\nProfile=Untitled\nProfileDir=Untitled\nSceneCollection=Test\nSceneCollectionFile=Test\n' > "$D/global.ini"
printf '[General]\nName=Untitled\n[Video]\nBaseCX=1280\nBaseCY=720\nOutputCX=1280\nOutputCY=720\nFPSCommon=30\n[Output]\nMode=Simple\n[SimpleOutput]\nFilePath=/tmp/rec\nRecFormat2=mkv\n' > "$D/basic/profiles/Untitled/basic.ini"
rm -rf "$D/logs"
LIBGL_ALWAYS_SOFTWARE=1 QT_QPA_PLATFORM=xcb timeout "${2:-60}" xvfb-run -a -s "-screen 0 1280x800x24" \
  obs --disable-shutdown-check --collection Test --startrecording >/tmp/obs.out 2>&1 || true
grep -hE "new cue|chapter|cue-chapters\] loaded" "$D"/logs/* || echo "no cue events found - see $D/logs"
