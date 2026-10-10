# Stand-ins for optional system libraries.
#
# The libraries listed in ./libraries belong to the target system: the
# display server's client library has to match the server, GPU buffer
# libraries belong to the graphics driver, and ALSA to the sound setup. They
# cannot be bundled, but they are also not always there. For each of them
# this builds a static archive of the same name whose functions load the real
# library on first use (tools/syslib-stubs). The engine links the archive, so
# it does not list the library as a dependency and starts without it.
#
# Headers and version numbers come from the host's development packages.

DEP_VERSION=1
DEP_SOURCES=()

dep_build() {
    local tools="$ZEBKIT_ROOT/tools/syslib-stubs"
    local multiarch hostlib
    multiarch="$(gcc -print-multiarch 2>/dev/null || echo x86_64-linux-gnu)"
    mkdir -p "$DEPS_PREFIX/lib/pkgconfig"

    "$DEP_CC" $DEP_CFLAGS -c "$tools/loader.c" -o loader.o || return
    ar rcs "$DEPS_PREFIX/lib/libzebkit-syslib.a" loader.o || return

    local name soname module default rest
    while read -r name soname module default rest; do
        [[ -z "$name" || "$name" == \#* ]] && continue
        hostlib=""
        local dir
        for dir in "/usr/lib/$multiarch" /usr/lib64 /usr/lib; do
            [[ -e "$dir/$soname" ]] && { hostlib="$dir/$soname"; break; }
        done
        [[ -n "$hostlib" ]] || { echo "host copy of $soname not found; install its development package" >&2; return 1; }

        local options=(--default "$default") option
        for option in $rest; do
            options+=("--${option%%=*}" "${option#*=}")
        done
        python3 "$tools/gen.py" "$soname" "$hostlib" "$name.S" "${options[@]}" || return
        "$DEP_CC" -c "$name.S" -o "$name.o" || return
        # The loader goes into every archive as well, so that each stand-in
        # links on its own (configure checks link one library at a time).
        local objects=("$name.o" loader.o)

        # libwayland-client also exports the core protocol's interface tables
        # as data, which a stub cannot forward. Generate our own copy from the
        # protocol description, as is done for every other Wayland protocol.
        if [[ "$name" == wayland-client ]]; then
            wayland-scanner private-code "$(pkg-config --variable=pkgdatadir wayland-scanner)/wayland.xml" wayland-core-protocol.c || return
            "$DEP_CC" $DEP_CFLAGS -fvisibility=hidden -c wayland-core-protocol.c -o wayland-core-protocol.o || return
            objects+=(wayland-core-protocol.o)
        fi
        rm -f "$DEPS_PREFIX/lib/lib$name.a"
        ar rcs "$DEPS_PREFIX/lib/lib$name.a" "${objects[@]}" || return

        # A pkg-config file with the host's version and include flags, but
        # pointing at the stand-in.
        local version cflags
        version="$(env -u PKG_CONFIG_PATH pkg-config --modversion "$module")" || return
        cflags="$(env -u PKG_CONFIG_PATH pkg-config --cflags-only-I "$module")"
        {
            echo "prefix=$DEPS_PREFIX"
            echo "libdir=\${prefix}/lib"
            env -u PKG_CONFIG_PATH pkg-config --print-variables "$module" 2>/dev/null | while read -r variable; do
                case "$variable" in prefix|libdir|exec_prefix|includedir|pcfiledir) continue ;; esac
                echo "$variable=$(env -u PKG_CONFIG_PATH pkg-config --variable="$variable" "$module")"
            done
            echo
            echo "Name: $module"
            echo "Description: lazy-loading stand-in for $soname"
            echo "Version: $version"
            echo "Libs: -L\${libdir} -l$name -lzebkit-syslib -ldl"
            echo "Cflags: $cflags"
        } > "$DEPS_PREFIX/lib/pkgconfig/$module.pc"
        echo "stand-in for $soname: $(nm "$name.o" | grep -c ' T ') functions"
    done < "$DEP_DIR/libraries"
}
