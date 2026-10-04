#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${OO_PS4_TOOLCHAIN:?OO_PS4_TOOLCHAIN must point to OpenOrbis PS4Toolchain}"

WORK="${ROOT}/.netsurf-work"
PREFIX="${ROOT}/.netsurf-ps4"
TOOLS="${WORK}/tools"
HOST="x86_64-pc-freebsd12-elf"
BUILD="x86_64-linux-gnu"

rm -rf "$WORK" "$PREFIX"
mkdir -p "$WORK/src" "$TOOLS" "$PREFIX/share/netsurf-buildsystem"

fetch_archive() {
    local repo="$1" sha="$2" name="$3"
    local tgz="$WORK/${name}.tar.gz"
    local dst="$WORK/src/${name}"
    echo "==> Fetching ${repo}@${sha}"
    curl -fL --retry 5 --retry-all-errors         "https://github.com/${repo}/archive/${sha}.tar.gz" -o "$tgz"
    mkdir -p "$dst"
    tar xzf "$tgz" -C "$dst" --strip-components=1
}

fetch_archive netsurf-browser/buildsystem 0005ae300283ff01c2e2b05e7376b3e55dea21f7 buildsystem
fetch_archive netsurf-browser/libwapcaplet c7c128d3eb3223b216c974471f82e9337fbcf4ba libwapcaplet
fetch_archive netsurf-browser/libparserutils 6b0cbf086ca8eb8fe74b69f0c9ecf274eb2397ca libparserutils
fetch_archive netsurf-browser/libhubbub 6651b8cf87a4aa87bcdb2ff024a02659cd3f9402 libhubbub
fetch_archive netsurf-browser/libcss 499f1c4601ad39942fd1b2204053a387bec9b989 libcss
fetch_archive netsurf-browser/libdom f69781e1f062444b5af3f62d431d7d94018da53b libdom
fetch_archive netsurf-browser/libnsutils 0bd39060740b6163bd50875326654a722df97eb2 libnsutils
fetch_archive netsurf-browser/libnslog bedff2146270a8a73cc265bab46ec39f9c170d07 libnslog
fetch_archive netsurf-browser/libnsbmp ea063c9f46acb43e90208da14073332b505ef7e7 libnsbmp
fetch_archive netsurf-browser/libnsgif 22e99eb6818b1284d0f3ff1b7f46159e87221220 libnsgif
fetch_archive netsurf-browser/libnsfb b701cdce7241c3747ccd78658a365db0983ebe24 libnsfb

cp -a "$WORK/src/buildsystem/." "$PREFIX/share/netsurf-buildsystem/"

cat > "$TOOLS/ps4-clang" <<EOF
#!/usr/bin/env bash
exec clang-18 --target=$HOST -fPIC -DORBIS -D_GNU_SOURCE \
  -isysroot "$OO_PS4_TOOLCHAIN" \
  -isystem "$OO_PS4_TOOLCHAIN/include" \
  "\$@"
EOF

cat > "$TOOLS/ps4-clang++" <<EOF
#!/usr/bin/env bash
exec clang++-18 --target=$HOST -fPIC -DORBIS -D_GNU_SOURCE \
  -isysroot "$OO_PS4_TOOLCHAIN" \
  -isystem "$OO_PS4_TOOLCHAIN/include" \
  -isystem "$OO_PS4_TOOLCHAIN/include/c++/v1" \
  "\$@"
EOF

cat > "$TOOLS/ps4-pkg-config" <<EOF
#!/usr/bin/env bash
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
exec pkg-config "\$@"
EOF

chmod +x "$TOOLS/ps4-clang" "$TOOLS/ps4-clang++" "$TOOLS/ps4-pkg-config"

COMMON_ARGS=(
    "PREFIX=$PREFIX"
    "NSSHARED=$PREFIX/share/netsurf-buildsystem"
    "HOST=$HOST"
    "BUILD=$BUILD"
    "VARIANT=release"
    "CC=$TOOLS/ps4-clang"
    "CXX=$TOOLS/ps4-clang++"
    "AR=llvm-ar-18"
    "RANLIB=llvm-ranlib-18"
    "PKGCONFIG=$TOOLS/ps4-pkg-config"
)

# Keep NetSurf's own include flags, but append PS4 compatibility after -Werror.
export CFLAGS="-Wno-error -I$ROOT/ports/compat"
export CXXFLAGS="-Wno-error -I$ROOT/ports/compat"

build_and_install() {
    local name="$1"
    echo
    echo "===== BUILD ${name} ====="
    (
        cd "$WORK/src/$name"
        make -j2 "${COMMON_ARGS[@]}"
        make "${COMMON_ARGS[@]}" install
    )
}

build_and_install libwapcaplet
build_and_install libparserutils
build_and_install libhubbub
build_and_install libcss

cat > "$WORK/src/libdom/Makefile.config.override" <<'EOF'
WITH_LIBXML_BINDING := no
WITH_EXPAT_BINDING := no
WITH_HUBBUB_BINDING := yes
EOF
build_and_install libdom

build_and_install libnsutils
build_and_install libnslog
build_and_install libnsbmp
build_and_install libnsgif

# libnsfb's upstream SDL surface is SDL 1.2. Inject the PS4 SDL2 surface
# instead and keep the portable RAM surface.
cp "$ROOT/ports/libnsfb/ps4_sdl2.c" "$WORK/src/libnsfb/src/surface/ps4_sdl2.c"
printf '\nSURFACE_HANDLER_yes += ps4_sdl2.c\n' >> "$WORK/src/libnsfb/src/surface/Makefile"
build_and_install libnsfb

echo
echo "===== PS4 NetSurf dependency prefix ====="
find "$PREFIX/include" -maxdepth 3 -type f | sort | sed -n '1,120p'
find "$PREFIX/lib" -maxdepth 2 -type f | sort
echo
"$TOOLS/ps4-pkg-config" --modversion libwapcaplet libparserutils libhubbub libcss libdom libnsutils libnslog libnsbmp libnsgif libnsfb
