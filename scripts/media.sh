#!/usr/bin/env bash
# Check that media actually plays in a built profile. tests/media/ holds four
# short generated samples (H.264/AAC in MP4, VP9/Opus in WebM, MP3, and an
# H.264 video with non-square pixels) and a page
# that plays each one muted and records what happened. A sample passes when
# play() resolves, playback reaches the end, and for video the decoded frame
# size is right. It needs no sound device and no network.
#
#   scripts/media.sh <profile>

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

profile="${1:-}"
require_profile "$profile"
stage_profile "$profile"

probe="$build_dir/tools/pageprobe"
[[ -x "$probe" ]] || die "no pageprobe for '$profile'; run scripts/compat.sh $profile <any site> once to build it"

port="${MEDIA_PORT:-8795}"
work="$(mktemp -d)"
python3 -u "$ZEBKIT_ROOT/tests/serve.py" "$ZEBKIT_ROOT/tests/media" "$port" > "$work/server.log" 2>&1 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null; rm -r "$work"' EXIT
sleep 1

log "playing the media samples with profile '$profile'"
"${run_env[@]}" timeout 120 "$probe" --settle 32 --timeout 60 --eval 'JSON.stringify(window.__media)' \
    "http://127.0.0.1:$port/index.html?run=$(date +%s%N)" > "$work/result.json" 2> "$build_dir/media.log" || true

python3 - "$work/result.json" <<'PY'
import json, sys
try:
    media = json.loads(json.load(open(sys.argv[1]))["page"])
except Exception as error:
    sys.exit(f"error: no result from the media page ({error})")

failed = 0
for name, r in media["samples"].items():
    problems = []
    if r.get("play") != "resolved":
        problems.append(f"play() {r.get('play')}")
    if "ended" not in r.get("events", []):
        problems.append(f"did not reach the end (stopped at {r.get('currentTime')} of {r.get('duration')} s)")
    if r.get("error"):
        problems.append(f"error {r['error']}")
    expected = (480, 240) if "anamorphic" in name else (320, 240)
    if "videoWidth" in r and (r["videoWidth"], r["videoHeight"]) != expected:
        problems.append(f"size {r['videoWidth']}x{r['videoHeight']}, expected {expected[0]}x{expected[1]}")
    print(f"  {'FAIL' if problems else 'PASS'}  {name}: canPlayType={r.get('canPlayType')!r}"
          + (", " + "; ".join(problems) if problems else f", played {r.get('duration')} s"))
    failed += bool(problems)
api = media["api"]
print(f"  Media Source: {api['MediaSource']}, H.264 {api['mseH264']}, VP9 {api['mseVP9']}; "
      f"Web Audio: {api['AudioContext']}; Media Session: {api['mediaSession']}")
sys.exit(1 if failed or not media.get("done") else 0)
PY
log "media playback OK"
