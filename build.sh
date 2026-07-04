#!/bin/bash
set -e

base="$(realpath $(dirname "$0"))"

builddir="$base/build"
pw_builddir="$base/build-pw"
pw_srcdir="$base/subprojects/pipewire-static"
enable_egl_backend="${SPOUT2PW_ENABLE_EGL_BACKEND:-false}"

prepare_builddir() {
    local dir="$1"
    local source_root="$2"
    local marker="$dir/.spout2pw-source-root"

    if [ -e "$dir"/build.ninja ]; then
        if [ ! -e "$marker" ] || [ "$(cat "$marker")" != "$source_root" ]; then
            rm -rf "$dir"
        fi
    fi

    mkdir -p "$dir"
}

mark_builddir() {
    local dir="$1"
    local source_root="$2"
    printf '%s\n' "$source_root" > "$dir/.spout2pw-source-root"
}

meson_setup_main() {
    if [ -e "$builddir"/build.ninja ]; then
        meson setup --reconfigure \
            --native-file "$builddir/native.txt" \
            --cross-file "$base"/misc/x86_64-w64-mingw32.txt \
            -Dlibpipewire_static_lib="$builddir/prefix/usr/lib/libpipewire-static-0.3.a" \
            -Denable_egl_backend="$enable_egl_backend" \
            "$builddir" "$base" || { cat "$builddir/meson-logs/meson-log.txt"; false; }
    else
        meson setup \
            --native-file "$builddir/native.txt" \
            --cross-file "$base"/misc/x86_64-w64-mingw32.txt \
            -Dlibpipewire_static_lib="$builddir/prefix/usr/lib/libpipewire-static-0.3.a" \
            -Denable_egl_backend="$enable_egl_backend" \
            "$builddir" "$base" || { cat "$builddir/meson-logs/meson-log.txt"; false; }
    fi
}

prepare_builddir "$builddir" "$base"
prepare_builddir "$pw_builddir" "$pw_srcdir"

if [ ! -e "$pw_builddir"/build.ninja ]; then
    meson setup "$pw_builddir" "$pw_srcdir" \
        -Dprefix="$builddir/prefix/usr" \
        -Dexamples=disabled \
        -Dtests=disabled \
        -Dgstreamer=disabled \
        -Dlibsystemd=disabled \
        -Dlogind=disabled \
        -Dselinux=disabled \
        -Dpipewire-alsa=disabled \
        -Dpipewire-jack=disabled \
        -Dpipewire-v4l2=disabled \
        -Dspa-plugins=enabled \
        -Dudev=disabled \
        -Dsdl2=disabled \
        -Dv4l2=disabled \
        -Dalsa=disabled \
        -Dx11=disabled \
        -Dlibffado=disabled \
        -Dsnap=disabled \
        -Dopus=disabled \
        -Dreadline=disabled \
        -Dgsettings=disabled \
        -Dsession-managers='[]' \
        -Ddefault_library=static \
        -Djack=disabled \
        -Davahi=disabled \
        -Dpipewire-alsa=disabled \
        -Draop=disabled -Davb=disabled \
        -Dlibpulse=disabled \
        -Dflatpak=disabled \
        -Dsupport=enabled \
        -Dstatic=true \
        -Dlibdir=lib
fi
mark_builddir "$pw_builddir" "$pw_srcdir"

echo "building"
ninja -C "$pw_builddir"
echo "installing"
ninja -C "$pw_builddir" install

cat > "$builddir/native.txt" <<EOF
[built-in options]
pkg_config_path='$builddir/prefix/usr/lib/pkgconfig'
EOF

meson_setup_main
mark_builddir "$builddir" "$base"
ninja -C "$builddir" install
