#!/usr/bin/env bash
# Build the ICU data file the package ships, out/blink/icu/icudtl.dat, for
# the languages zebkit supports: English, Chinese, Japanese and Korean.
#
# It is Chromium's own data recipe (third_party/icu/filters/common.json,
# which says what Chromium and V8 need of ICU) with the list of languages
# cut down, built with ICU's tools from the ICU source in the Chromium tree,
# so that the data matches the ICU compiled into the engine. Chromium ships
# its data prebuilt; this is how it makes it (third_party/icu/scripts/make_data_all.sh).
#
#   scripts/build-icu-data.sh

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

LANGUAGES=(root en zh ja ko)

icu="$SRC_DIR/third_party/icu"
build="$CACHE_DIR/icu-build"
out="$OUT_DIR/icu"
filter="$build/filter.json"
stamp="$out/.stamp"

want="$CHROMIUM_VERSION ${LANGUAGES[*]} $(sha256 "$0" | cut -d' ' -f1)"
if [[ -f "$stamp" && "$(cat "$stamp")" == "$want" && -f "$out/icudtl.dat" ]]; then
    log "ICU data is up to date: $out/icudtl.dat"
    exit 0
fi

rm -rf "$build"
mkdir -p "$build" "$out"

# Chromium's filter is JSON with // comments. Its feature filters are kept;
# only the languages change: the overall list, and the lists of their own
# that the trees of time zone, currency, unit, region and collation names
# and of transliterators have, which take precedence over it. Entries of
# those lists that are not languages (tzdbNames) stay.
python3 -I - "$icu/filters/common.json" "$filter" "${LANGUAGES[@]}" <<'EOF'
import json, re, sys
source, target, languages = sys.argv[1], sys.argv[2], sys.argv[3:]
text = "\n".join(line for line in open(source) if not line.lstrip().startswith("//"))
spec = json.loads(text)
chromium_languages = set(spec["localeFilter"]["includelist"])
spec["localeFilter"] = {"filterType": "language", "includelist": languages}

def narrow(f):
    if f.get("filterType") == "language":
        f["includelist"] = [x for x in f["includelist"]
                            if x in languages or x not in chromium_languages]
    for g in f.get("unionOf", []):
        narrow(g)

for f in spec.get("featureFilters", {}).values():
    if isinstance(f, dict):
        narrow(f)
json.dump(spec, open(target, "w"), indent=2)
EOF

# ICU's build writes into the source tree's data directory; build from a
# copy so that the Chromium tree stays as fetched.
log "copying the ICU source"
cp -a "$icu/source" "$build/source"
# Pseudo-locales, which Chromium's recipe also removes.
find "$build/source/data" \( -name 'en_XA*' -o -name 'ar_XB*' \) -delete

log "building ICU's tools and the filtered data"
mkdir -p "$build/obj"
icu_platform=Linux/gcc
if [[ "$ZEBKIT_HOST" == macos ]]; then
    icu_platform=MacOSX
    jobs="$(sysctl -n hw.ncpu)"
else
    jobs="$ZEBKIT_JOBS"
fi
(cd "$build/obj" &&
    ICU_DATA_FILTER_FILE="$filter" "$build/source/runConfigureICU" --enable-debug --disable-release \
        "$icu_platform" --disable-tests --disable-layoutex --disable-samples --enable-rpath \
        --prefix="$build/prefix" > "$build/configure.log" 2>&1 &&
    heavy make -j "$jobs" > "$build/make.log" 2>&1) || die "ICU build failed; see $build/make.log"

dat="$(find "$build/obj/data/out/tmp" -name 'icudt*l.dat' | head -1)"
[[ -n "$dat" ]] || die "no data file in $build/obj/data/out/tmp"

# The last item of the data's table of contents has no length (ucmndata.cpp's
# offsetTOCLookupFn), and V8's Temporal reads zoneinfo64.res with its length
# (js-temporal-zoneinfo64.cc): when it is last, Temporal has no time zones.
# Chromium's data has zu.res after it; without Zulu, an item of no other use
# follows it (a copy of a small resource, under a name no locale has).
# ICU's own libraries, beside its tools: LD_LIBRARY_PATH on Linux, DYLD_LIBRARY_PATH
# on a Mac, which ignores the other.
icupkg() { LD_LIBRARY_PATH="$build/obj/lib" DYLD_LIBRARY_PATH="$build/obj/lib" "$build/obj/bin/icupkg" "$@"; }
items="$(icupkg --ignore-deps -l "$dat")" || die "icupkg could not list $dat"
if [[ "$(tail -1 <<< "$items")" == zoneinfo64.res ]]; then
    pad="$build/pad"
    rm -rf "$pad" && mkdir -p "$pad"
    # The filtered data's own dependencies are left as they are.
    icupkg --ignore-deps -d "$pad" -x res_index.res "$dat"
    mv "$pad/res_index.res" "$pad/zzzz_after_zoneinfo64.res"
    icupkg --ignore-deps -s "$pad" -a zzzz_after_zoneinfo64.res "$dat"
    [[ "$(icupkg --ignore-deps -l "$dat" | tail -1)" != zoneinfo64.res ]] \
        || die "zoneinfo64.res is still the data's last item"
fi
cp "$dat" "$out/icudtl.dat"
echo "$want" > "$stamp"
log "$(wc -c < "$out/icudtl.dat" | awk '{ printf "%.1f", $1 / 1048576 }') MiB: $out/icudtl.dat"
