#!/usr/bin/env bash
# Run the engine on glibc 2.17, the oldest it supports, as CentOS 7 has it.
#
#   scripts/glibc-test.sh <profile>
#   scripts/glibc-test.sh --root    # only lay out glibc 2.17's root (below)
#
# Without a container: CentOS 7's own glibc, libraries and headers are
# unpacked into out/blink/glibc-2.17/root (once; about 400 MB), the C tests
# are built against that root with Chromium's clang, and their programs
# and zebkit-helper, in a copy of the package, are given its dynamic
# linker. Every process of the engine then runs on glibc 2.17 and
# CentOS 7's libraries, and on nothing of this machine's but the kernel.
#
# The tests are engine-test.sh's C ones, headless and without the sandbox.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

# --root: only lay out the root, which scripts/release.sh links the tools against.
profile="${1:-}"
[[ "$(uname)" == Linux ]] || die "glibc is Linux's"
if [[ "$profile" != --root ]]; then
    require_profile "$profile"
    package="${ZEBKIT_TEST_PACKAGE:-$OUT_DIR/$profile/package}"
    [[ -f "$package/libzebkit.so.1" ]] || die "no engine in $package; run scripts/package.sh $profile first"
fi

cache="$OUT_DIR/glibc-2.17"
root="$cache/root"
clang="$SRC_DIR/third_party/llvm-build/Release+Asserts/bin/clang"

# CentOS 7's base image, and the packages it lacks that the engine loads
# (and the compiler needs), from its final release, 7.9.2009.
image_layer=sha256:2d473b07cdd5f0912cd6f1a703352c82b512407db6b05b43f2553732b55df3bc
vault=https://vault.centos.org/7.9.2009/os/x86_64/Packages
rpms=(
    "alsa-lib-1.1.8-1.el7.x86_64.rpm 01990da86e8971970800575c029714a2fc6c8245c20085a68c89cec3955b1618"
    "atk-2.28.1-2.el7.x86_64.rpm 942fc4bc5390444c1758043bc5dd4c01c84e3e7c008c1c8ad4a6aefffb5e458d"
    "at-spi2-atk-2.26.2-1.el7.x86_64.rpm 200b24d1dd03fd72979a2aa76bfa8152da9c86fde1247f0848f0282bd26eb495"
    "at-spi2-core-2.28.0-1.el7.x86_64.rpm 24fb8f259f1c458490f2b6413d92bfdd781c01d5f13b291324bcceeea519e3c4"
    "cpp-4.8.5-44.el7.x86_64.rpm 0ded8c80b8f3a1d4ca94037eca3438f3215e7b031c97479cee121bcbb6249e29"
    "gcc-4.8.5-44.el7.x86_64.rpm 186d9bdbb568d08c4d8b2f9a2c0fce952c2ac80ef5989806116df61c7cbc5a22"
    "glibc-devel-2.17-317.el7.x86_64.rpm 57e04523b12c9aa5a0632cbfa2ff13e33f1ecec5e6bd0b609a77d9c24cebf8c4"
    "glibc-headers-2.17-317.el7.x86_64.rpm b7e4e19b362cbd73e09e6ee5eff3d70dbeb7cc2c702a927b8f646f324d4ec4a3"
    "kernel-headers-3.10.0-1160.el7.x86_64.rpm 81b4e4f401d2402736ceba4627eaafd5b615c2cc45aa4d4f941ea79562045139"
    "libdrm-2.4.97-2.el7.x86_64.rpm cc11c6badf5727a5d0d3153df66de9a7f9a5957540beb085dc1915a231adbe7f"
    "libgcc-4.8.5-44.el7.x86_64.rpm cb1967fba485ad3da345e33018c0e8b367359f251c6b1a8847a4976579d74ffa"
    "libwayland-server-1.15.0-1.el7.x86_64.rpm 81bd39be198a5c7bd27bd9b096b8ffb201916e6a0575a89c77bfd6b32224efd7"
    "libX11-1.6.7-2.el7.x86_64.rpm a8727ec247d6a14fea9c24707280fc299e9eef6e499a1e1e41992b4fa9bdf7a1"
    "libX11-common-1.6.7-2.el7.noarch.rpm dc2895887d4354cff78b1992896dd6b925602a13134321b0be45551c022bc420"
    "libXau-1.0.8-2.1.el7.x86_64.rpm c6441b423c19c5ca543d72fe6ed3679f039538ad345cb4284a36781869f8609c"
    "libxcb-1.13-1.el7.x86_64.rpm 125d6cdbea06799872b77b6322bf5cb107d7c0c70218827bf6b649f08ca527c5"
    "libXcomposite-0.4.4-4.1.el7.x86_64.rpm 81357f494edca6c828f541cf9d229d97ac5e496fbb23a28a86535b5b9cd49a41"
    "libXdamage-1.1.4-4.1.el7.x86_64.rpm 790df35e712320c967e7934e417687e17457b7cf989e54045115dd8f4c560949"
    "libXext-1.3.3-3.el7.x86_64.rpm 58284e32f1f3343c56722653e7870e8062c1570cd965802bc284da6480d9d366"
    "libXfixes-5.0.3-1.el7.x86_64.rpm 23a99007971fcbe272a4a82aa79bc49a46e573eecf696daedd84dd7959b2344d"
    "libXi-1.7.9-1.el7.x86_64.rpm 7ef78114fabd31ab69aaf5635b3845e10598b3c172759dc65b135f7cef624342"
    "libxkbcommon-0.7.1-3.el7.x86_64.rpm a9b062f6eb21386a67dfea0fd3f3bca884fa25a938f6edf36b4a4fcc30578fc9"
    "libXrandr-1.5.1-2.el7.x86_64.rpm 68e7000c212cd6943a71daa3633317f1637533be0df13e929f0202fc22726b83"
    "libXrender-0.9.10-1.el7.x86_64.rpm 3de0d3812ff49c6e10175cc56c30fa1801d158e6fb34cb1cdb245ef466bae173"
    "libXtst-1.2.3-1.el7.x86_64.rpm fb26618b371e304889912c79aa009388aaa17f3b08fdce9512d70f3957fe18e2"
    "mesa-libgbm-18.3.4-10.el7.x86_64.rpm ccf388cccbcd260558a99df6901cd3ca04b2fec8557e3e80dfa88e7089f2df04"
    "mesa-libglapi-18.3.4-10.el7.x86_64.rpm 40466132f3f6b62777ad27df7dc0060f99a362c55f00ed125d0f58437066a38f"
)
# patchelf, from its Python wheel: nothing to install on the machine.
patchelf_wheel="patchelf-0.19.1.0-py3-none-manylinux1_x86_64.manylinux_2_5_x86_64.musllinux_1_1_x86_64.whl"
patchelf_sha256=a8f6331ccf40c345507279f755f4a38c2cb00b9efda746fd43c17713cce0aba4

# fetch <url> <file> <sha256> [curl options...]
fetch() {
    local url="$1" file="$2" sum="$3"
    shift 3
    if [[ ! -f "$file" ]] || [[ "$(sha256 "$file" | cut -d' ' -f1)" != "$sum" ]]; then
        curl -fsSL "$@" -o "$file.part" "$url"
        [[ "$(sha256 "$file.part" | cut -d' ' -f1)" == "$sum" ]] || die "$url is not the file expected"
        mv "$file.part" "$file"
    fi
}

if [[ ! -f "$root/.ready" ]]; then
    log "unpacking CentOS 7 into $root"
    mkdir -p "$cache/downloads"
    token="$(curl -fsSL 'https://auth.docker.io/token?service=registry.docker.io&scope=repository:library/centos:pull' \
        | python3 -I -c 'import json, sys; print(json.load(sys.stdin)["token"])')"
    fetch "https://registry-1.docker.io/v2/library/centos/blobs/$image_layer" "$cache/downloads/centos-7.tar.gz" \
        "${image_layer#sha256:}" -H "Authorization: Bearer $token"
    for entry in "${rpms[@]}"; do
        read -r name sum <<< "$entry"
        fetch "$vault/$name" "$cache/downloads/$name" "$sum"
    done
    fetch "https://files.pythonhosted.org/packages/py3/p/patchelf/$patchelf_wheel" "$cache/downloads/$patchelf_wheel" \
        "$patchelf_sha256"

    rm -rf "$root"
    mkdir -p "$root"
    tar -xzf "$cache/downloads/centos-7.tar.gz" -C "$root" 2>/dev/null || true
    chmod -R u+w "$root"
    python3 -I "$ZEBKIT_ROOT/scripts/rpm-extract.py" "$root" \
        $(for entry in "${rpms[@]}"; do echo "$cache/downloads/${entry%% *}"; done)
    # Links that name a path of CentOS's own, such as /lib64/libgcc_s.so.1,
    # to that path inside the root.
    find "$root/usr/lib64" "$root/usr/lib/gcc" -lname '/*' | while read -r link; do
        ln -sfn "$root$(readlink "$link")" "$link"
    done
    rm -rf "$cache/patchelf"
    python3 -I -m zipfile -e "$cache/downloads/$patchelf_wheel" "$cache/patchelf"
    chmod +x "$cache/patchelf/patchelf-0.19.1.0.data/scripts/patchelf"
    touch "$root/.ready"
fi
patchelf="$cache/patchelf/patchelf-0.19.1.0.data/scripts/patchelf"
loader="$root/usr/lib64/ld-2.17.so"
[[ -x "$patchelf" && -f "$loader" ]] || die "$root is incomplete; remove it and run this again"
[[ "$profile" == --root ]] && exit 0

work="$(mktemp -d)"
pids=()
trap 'for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done; rm -rf "$work"' EXIT

cp -a "$package" "$work/package"
"$patchelf" --set-interpreter "$loader" "$work/package/zebkit-helper"

log "building the C tests against glibc 2.17"
tests="$ZEBKIT_ROOT/tests/engine"
build() {
    local name="$1" source="$2"
    shift 2
    # shellcheck disable=SC2046
    # Warnings are engine-test.sh's business, with the system compiler.
    "$clang" -std=c11 -O1 -w --sysroot="$root" --gcc-toolchain="$root/usr" -fuse-ld=lld \
        -I"$work/package/include" "$source" "$@" $(engine_link_options "$work/package") -o "$work/$name"
    "$patchelf" --set-interpreter "$loader" "$work/$name"
}
for name in vendor services pdf bridge page popup scheme media mode hooks certificate http_auth hook_policies offscreen_scale; do
    build "$name" "$tests/$name.c"
done
build api "$tests/api.c" -pthread
build native-window "$tests/native_window.c" -pthread
"$clang" -std=c11 -O1 -Wall -Werror --sysroot="$root" --gcc-toolchain="$root/usr" -fuse-ld=lld \
    -I"$work/package/include" "$tests/late_load.c" -ldl -pthread -o "$work/late-load"
"$patchelf" --set-interpreter "$loader" "$work/late-load"


# What each program needs of glibc, as built.
newest="$(for file in "$work"/* "$work/package"/*; do
    [[ -f "$file" ]] && objdump -T "$file" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' || true
done | sed 's/GLIBC_//' | sort -uV | tail -1)"
[[ "$newest" == 2.17 || "$(printf '%s\n' 2.17 "$newest" | sort -V | tail -1)" == 2.17 ]] \
    || die "something here needs glibc $newest"

failed=0
engine_switches=(--no-sandbox --headless --user-data-dir="$work/profile")
# Only CentOS's libraries: none of this machine's, and nothing preloaded.
system=(env -u LD_PRELOAD LD_LIBRARY_PATH="$work/package:$root/usr/lib64")

run() {
    local name="$1"
    shift
    log "$name"
    if ! limited 180 "${system[@]}" "$@" > "$work/$name.log" 2> "$work/$name.err"; then
        failed=1
        cat "$work/$name.log"
        echo "it printed on stderr:"; tail -20 "$work/$name.err"
    else
        cat "$work/$name.log"
    fi
}
finished() {
    grep -q '^\([0-9]*\) of \1 checks passed$' "$work/$1.log" || { echo "FAIL $1 did not finish"; failed=1; }
}

log "the C library the engine runs on"
"${system[@]}" "$loader" "$root/usr/lib64/libc.so.6" | head -1

run vendor "$work/vendor" "$SRC_DIR/third_party/pdfium/testing/resources/fonts/ahem/Ahem.ttf"
run services "$work/services"
run pdf "$work/pdf"
run api "$work/api" "${engine_switches[@]}"
run mode-headless "$work/mode" "${engine_switches[@]}"
for owner in main worker; do
    run "late-load-$owner" "$work/late-load" "$work/package/libzebkit.so.1" "$owner" "${engine_switches[@]}"
done


# On a port that was free, and that this server answers on.
start_page_server "$tests/pages" "$work/server.log"
pids+=("$page_server")
port="$page_port"
base="http://127.0.0.1:$port"
mkdir -p "$work/files" "$work/page-files" "$work/media-files" "$work/scheme-files"
ZEBKIT_TEST_BASE="$base" ZEBKIT_TEST_OTHER="http://localhost:$port" ZEBKIT_TEST_FILES="$work/files" \
    run bridge "$work/bridge" "${engine_switches[@]}"
ZEBKIT_TEST_BASE="$base" ZEBKIT_TEST_FILES="$work/page-files" run page "$work/page" "${engine_switches[@]}"
finished page
run hooks "$work/hooks" "${engine_switches[@]}"
run offscreen-scale "$work/offscreen_scale" "${engine_switches[@]}"
run native-window "$work/native-window" "${engine_switches[@]}"
# The Python TLS server uses the host runtime; only its C child gets CentOS libc.
log "certificate-c"
if ! limited 180 python3 "$tests/certificate_c.py" "$work/certificate" \
    "$work/package:$root/usr/lib64" > "$work/certificate.log" 2> "$work/certificate.err"; then
    failed=1
    cat "$work/certificate.log"
    tail -20 "$work/certificate.err"
else
    cat "$work/certificate.log"
fi
log "http-auth-c"
if ! limited 180 python3 "$tests/http_auth_c.py" "$work/http_auth" \
    "$work/package:$root/usr/lib64" > "$work/http-auth.log" 2> "$work/http-auth.err"; then
    failed=1
    cat "$work/http-auth.log"
    tail -20 "$work/http-auth.err"
else
    cat "$work/http-auth.log"
fi
log "hook-policies"
if ! PYTHONPATH="$tests" ZEBKIT_HOOK_CHILD_LIBRARY_PATH="$work/package:$root/usr/lib64" limited 180 \
    python3 "$tests/hook_policies.py" c:download "$work/hook_policies" > "$work/hook-policies.log" 2> "$work/hook-policies.err"; then
    failed=1
    cat "$work/hook-policies.log"
    tail -20 "$work/hook-policies.err"
else
    cat "$work/hook-policies.log"
fi
ZEBKIT_TEST_BASE="$base" run popup "$work/popup" "${engine_switches[@]}"
finished popup
ZEBKIT_TEST_BASE="$base" ZEBKIT_TEST_FILES="$work/media-files" \
    run media "$work/media" "${engine_switches[@]}" --autoplay-policy=no-user-gesture-required
finished media
ZEBKIT_TEST_FILES="$work/scheme-files" run scheme "$work/scheme" "${engine_switches[@]}"

if (( failed )); then die "the engine fails on glibc 2.17"; fi
log "the engine runs on glibc 2.17"
