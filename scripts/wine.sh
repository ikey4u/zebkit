#!/usr/bin/env bash
# Run a Windows program from a profile's build on this machine, under Wine.
#
#   scripts/wine.sh <profile> <program> [arguments...]
#   scripts/wine.sh baseline jsc.exe -e 'print(1 + 1)'
#   scripts/wine.sh --debug baseline MiniBrowser.exe     # Wine's debugger; "bt" has symbols
#   scripts/wine.sh --stop                               # end every program started here
#
# The program is looked for in out/windows/<profile>/bin, and finds the
# libraries vcpkg built beside it. There is no window to see: the display
# is a virtual one (Xvfb), so that nothing appears on this machine's
# screen. Wine's own state is kept in out/toolchain/windows/wineprefix.
#
# Wine is not Windows. What runs here says the build is sound; what fails
# here may be Wine's doing.

ZEBKIT_TARGET=windows
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

wine="$WINDOWS_TOOLCHAIN_DIR/wine/bin"
[[ -x "$wine/wine" ]] || die "no Wine; run scripts/fetch-toolchain-windows.sh first"
export WINEPREFIX="$WINDOWS_TOOLCHAIN_DIR/wineprefix"

# A Windows program is several processes that this script's own is not the
# parent of, so ending the script ends none of them. Wine's server knows
# them all.
if [[ "${1:-}" == --stop ]]; then
    [[ -d "$WINEPREFIX" ]] && WINEDEBUG=-all "$wine/wineserver" -k 2>/dev/null || true
    exit 0
fi

debug=0
[[ "${1:-}" == --debug ]] && { debug=1; shift; }
profile="${1:-}"
require_profile "$profile"
program="${2:-}"
[[ -n "$program" ]] || die "usage: $(basename "$0") [--debug] <profile> <program> [arguments...]"
shift 2

bin="$OUT_DIR/$profile/bin"
[[ -f "$bin/$program" ]] || die "no $program in $bin; run ZEBKIT_TARGET=windows scripts/build.sh $profile"

# Quiet unless asked: WINEDEBUG=+seh and the like still work.
export WINEDEBUG="${WINEDEBUG:--all}"
# Wine would offer to install its .NET and HTML engines, in a window.
export WINEDLLOVERRIDES="mscoree,mshtml=${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
# Where a program started by another finds the libraries: Windows looks
# beside the program first and then here.
export WINEPATH="Z:$OUT_DIR/$profile/vcpkg_installed/x64-windows-zebkit/bin${WINEPATH:+;$WINEPATH}"
# Wine goes to the X server of DISPLAY, which xvfb-run sets.
unset WAYLAND_DISPLAY

run=(xvfb-run -a -s "-screen 0 1280x1024x24")
if [[ ! -d "$WINEPREFIX" ]]; then
    log "setting up Wine in $WINEPREFIX"
    "${run[@]}" "$wine/wineboot" -u
fi
# Wine has none of the fonts every Windows has, and the engine asks for
# them by name: for its last resort (Times New Roman) and for the text of
# form controls (the dialog font). Without them such text has no height.
# The Liberation fonts of this machine, which have the same measures, stand
# in for them.
if [[ ! -f "$WINEPREFIX/.zebkit-fonts" ]]; then
    fc-list : family | grep -q '^Liberation Sans' || die "the Liberation fonts are not installed (fonts-liberation)"
    while IFS='=' read -r name substitute; do
        "${run[@]}" "$wine/wine" reg add 'HKCU\Software\Wine\Fonts\Replacements' /v "$name" /t REG_SZ /d "$substitute" /f > /dev/null
    done <<'FONTS'
Times New Roman=Liberation Serif
Arial=Liberation Sans
Courier New=Liberation Mono
MS Shell Dlg=Liberation Sans
MS Shell Dlg 2=Liberation Sans
Segoe UI=Liberation Sans
Microsoft Sans Serif=Liberation Sans
FONTS
    "$wine/wineserver" -k 2> /dev/null || true
    touch "$WINEPREFIX/.zebkit-fonts"
fi

cd "$bin"
if (( debug )); then
    exec "${run[@]}" "$wine/winedbg" "Z:$bin/$program" "$@"
fi
exec "${run[@]}" "$wine/wine" "$program" "$@"
