#!/usr/bin/env bash
# Plays a measured session on the phone without anyone touching it: connects adb (wireless
# debugging, by mDNS), optionally installs an APK, opens the launcher, picks the game, starts it,
# presses Cross until the game is in gameplay (judged from the draws per frame in the 5-second
# statistics), measures for a while, prints a summary, pulls the logs, then closes the game and
# brings the Claude app back (the owner often follows the session on the same phone).
#
#   tools/android/autorun.sh [--game uc1|uc2|uc3] [--seconds 120] [--apk FILE] [--out DIR]
#                            [--env 'NAME=value NAME=value'] [--no-gameplay]
#
# --env rewrites uncharted-env.txt in the game folder (one line per NAME=value; '' empties it).
# The launcher's game tab and LAUNCH are found by text in a UI dump (either orientation); Cross is
# a gamepad key event, whatever the on-screen layout. Needs the phone unlocked; if the launcher
# does not come to the front, nothing is started.
set -u
ADB=${ADB:-adb}
game=uc2 seconds=120 apk= out="logs-$(date +%Y%m%d-%H%M%S)" env_lines=unset gameplay=1
while (($#)); do
    case $1 in
        --game) game=$2; shift ;; --seconds) seconds=$2; shift ;; --apk) apk=$2; shift ;;
        --out) out=$2; shift ;; --env) env_lines=$2; shift ;; --no-gameplay) gameplay=0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
export MSYS_NO_PATHCONV=1 # Git Bash: keep /sdcard/... paths as they are
pkg=com.uncharted.android
game_dir='/storage/emulated/0/games/Uncharted The Nathan Drake Collection/Game'
case $game in uc1) tab="Uncharted 1" ;; uc2) tab="Uncharted 2" ;; uc3) tab="Uncharted 3" ;; *) echo "bad --game" >&2; exit 2 ;; esac

log() { echo "[$(date +%H:%M:%S)] $*"; }
connected() { "$ADB" get-state >/dev/null 2>&1; }
connect() {
    connected && return 0
    for ep in $("$ADB" mdns services | awk '/_adb-tls-connect/ {print $3}'); do
        "$ADB" connect "$ep" >/dev/null 2>&1; sleep 2; connected && return 0
    done
    return 1
}
game_log() { "$ADB" logcat -d -s UnchartedGame:I | sed 's/^.*UnchartedGame: //'; }
back_to_chat() {
    "$ADB" shell am force-stop $pkg
    "$ADB" shell monkey -p com.anthropic.claude -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
    log "game closed, Claude app reopened"
}
# Taps the launcher control showing `text`, found in a UI dump: the launcher may come up in
# portrait or landscape (the phone is often still rotated from the game).
tap_text() {
    "$ADB" shell uiautomator dump /sdcard/autorun-ui.xml >/dev/null 2>&1
    local b; b=$("$ADB" shell cat /sdcard/autorun-ui.xml | grep -oE "(text|content-desc)=\"$1\"[^>]*bounds=\"[^\"]*\"" |
        head -1 | sed -E 's/.*bounds="\[([0-9]+),([0-9]+)\]\[([0-9]+),([0-9]+)\]"/\1 \2 \3 \4/')
    [ -n "$b" ] || { log "'$1' not found on screen"; return 1; }
    set -- $b
    "$ADB" shell input tap $((($1 + $3) / 2)) $((($2 + $4) / 2))
}
press_cross() { "$ADB" shell input gamepad keyevent --duration 150 KEYCODE_BUTTON_A; }
last_draws() { game_log | sed -n 's/^Frame stats: .* \([0-9][0-9]*\) draws\/frame.*/\1/p' | tail -1; }

for _ in $(seq 1 20); do connect && break; log "phone not reachable, retrying"; sleep 15; done
connect || { log "phone not reachable over adb"; exit 1; }
trap back_to_chat EXIT
"$ADB" shell am force-stop $pkg
if [ -n "$apk" ]; then log "installing $apk"; "$ADB" install -r "$apk" | tail -1; fi
if [ "$env_lines" != unset ]; then
    content=""
    for kv in $env_lines; do content+="$kv\\n"; done # values never hold spaces or quotes
    "$ADB" shell "printf '$content' > '$game_dir/uncharted-env.txt'"
    log "uncharted-env.txt: $("$ADB" shell cat "'$game_dir/uncharted-env.txt'" | tr '\n' ' ')"
fi
"$ADB" shell input keyevent KEYCODE_WAKEUP
"$ADB" shell am start -W -n $pkg/com.gow3.android.MainActivity >/dev/null; sleep 3
if ! "$ADB" shell dumpsys activity activities | grep -m1 -E "topResumedActivity" | grep -q "$pkg/com.gow3.android.MainActivity"; then
    log "the launcher is not in front (phone locked?): not starting the game"; exit 1
fi
tap_text "$tab" || exit 1; sleep 1
"$ADB" logcat -c
tap_text "LAUNCH" || exit 1
sleep 15
game_log | grep -q "^Uncharted: " || { log "the game did not start after LAUNCH"; exit 1; }
log "started $game"
start=$(date +%s) in_game=0 presses=0 last_press=0
while :; do
    sleep 5
    now=$(($(date +%s) - start))
    if game_log | grep -q "^STOP:"; then log "the game stopped (crash) at ${now}s"; break; fi
    draws=$(last_draws); draws=${draws:-0}
    if ((gameplay && !in_game)); then
        # The logos draw 9-25 per frame, the title menus 80-90 (Uncharted 1) and 140-300
        # (Uncharted 2); gameplay 450 and up. Cross twice (past "press start", then Continue)
        # starts the game, after which it loads: press again only if 30 s bring no gameplay.
        if ((draws >= 400)); then in_game=$now; log "gameplay at ${now}s ($draws draws/frame) after $presses presses"
        elif ((now >= 240)); then log "no gameplay after 4 minutes ($draws draws/frame): measuring where it is"; in_game=$now
        elif ((draws >= 30 && (presses % 2 == 1 || now - last_press >= 30))); then
            press_cross; presses=$((presses + 1)); last_press=$now; log "Cross ($presses) at ${now}s, $draws draws/frame"
        fi
    elif ((!gameplay && !in_game && now >= 30)); then in_game=$now
    fi
    ((in_game && now - in_game >= seconds)) && break
done
game_log | grep -E "^Uncharted: |^Job spin|^uncharted-env|^Frame stats|process CPU|^Guest sync/s|^STOP:|Device lost|Unreachable" |
    sed -E 's/^(Frame stats: [0-9.]+ FPS).*( [0-9]+ draws\/frame).*/\1;\2/; s/^CPU clocks.*(capped to [0-9. ]+).*(process CPU.*)/  \1 \2/'
mkdir -p "$out" && "$ADB" pull "/sdcard/Android/data/$pkg/files/logs/." "$out" | tail -1
# Shader dumps, when the run had GOW3_DUMP_SHADERS=1 GOW3_SHADER_DUMP_DIR=<that folder>.
shaders="/sdcard/Android/data/$pkg/files/shaders"
if "$ADB" shell "[ -d $shaders ]"; then mkdir -p "$out/shaders" && "$ADB" pull "$shaders/." "$out/shaders" | tail -1; fi
log "logs in $out"
