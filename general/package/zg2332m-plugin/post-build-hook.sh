#!/bin/sh
# Post-build hook: assemble a standalone tarball of the zg2332m plugin so
# it can be dropped onto a camera that is already flashed with a stock
# openipc build. Runs from rootfs_script.sh after the target rootfs is
# assembled (see general/scripts/late-post-build-hooks.list).
#
# Arg1: TARGET_DIR (the assembled rootfs, buildroot's output/target)
set -eu
TARGET_DIR="${1:-}"
[ -n "$TARGET_DIR" ] || { echo "post-build-hook.sh: TARGET_DIR arg missing"; exit 1; }

SO="$TARGET_DIR/usr/lib/hisilicon.so"
CONF="$TARGET_DIR/etc/majestic-ae.conf"

if [ ! -f "$SO" ] || [ ! -f "$CONF" ]; then
    echo "post-build-hook.sh: plugin artefacts missing under $TARGET_DIR, skipping"
    exit 0
fi

OUT_DIR="$(dirname "$TARGET_DIR")/images"
mkdir -p "$OUT_DIR"

VER="${OPENIPC_VERSION:-$(date -u +%y.%m.%d)}"
STAGE="$(mktemp -d)"
PKGDIR="$STAGE/zg2332m-plugin-$VER"
mkdir -p "$PKGDIR"

cp "$SO"   "$PKGDIR/hisilicon.so"
cp "$CONF" "$PKGDIR/majestic-ae.conf"

cat > "$PKGDIR/install.sh" <<'EOF'
#!/bin/sh
# Install the ZG2332M plugin onto an already-flashed OpenIPC cam
# (Hi3516EV100 + SC2235P). Run this ON the camera as root.
#
# Two install modes:
#   RAM    (default) - bind-mount from /tmp, wiped on reboot.
#   PERSIST=1        - copy into the jffs2 overlay, survives reboot.
#                      Fails if the overlay is saturated.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -f "$HERE/hisilicon.so" ] && [ -f "$HERE/majestic-ae.conf" ] || {
    echo "install.sh: missing files next to this script"; exit 2;
}

if [ "${PERSIST:-0}" = "1" ]; then
    cp "$HERE/hisilicon.so"    /usr/lib/hisilicon.so
    cp "$HERE/majestic-ae.conf" /etc/majestic-ae.conf
    grep -q '^plugins: true' /etc/majestic.yaml 2>/dev/null || \
        echo 'plugins: true' >> /etc/majestic.yaml
    echo "installed persistently in /usr/lib and /etc"
else
    # RAM install via bind-mount (does not touch the overlay)
    mkdir -p /tmp/usrlib
    for f in /rom/usr/lib/*; do
        [ -e "/tmp/usrlib/$(basename "$f")" ] || ln -sf "$f" "/tmp/usrlib/$(basename "$f")"
    done
    cp "$HERE/hisilicon.so" /tmp/usrlib/hisilicon.so
    cp "$HERE/majestic-ae.conf" /tmp/majestic-ae.conf
    mountpoint -q /usr/lib || mount --bind /tmp/usrlib /usr/lib
    grep -q '^plugins: true' /etc/majestic.yaml 2>/dev/null || {
        cp /etc/majestic.yaml /tmp/majestic.yaml
        echo 'plugins: true' >> /tmp/majestic.yaml
        mount --bind /tmp/majestic.yaml /etc/majestic.yaml
    }
    echo "installed in RAM (bind-mounts) — will be wiped on reboot"
fi

/etc/init.d/S95majestic restart
sleep 5
echo "plugin commands: $(echo help | nc -w 3 localhost 4000 | head -1)"
EOF
chmod +x "$PKGDIR/install.sh"

cat > "$PKGDIR/README.md" <<EOF
# zg2332m-plugin $VER

Plugin AE Majestic pour Zosi ZG2332M (Hi3516EV100 + SC2235P).

## Contenu

- \`hisilicon.so\` — binaire à installer en \`/usr/lib/hisilicon.so\`
- \`majestic-ae.conf\` — config INI à installer en \`/etc/majestic-ae.conf\`
- \`install.sh\` — script d'installation (RAM par défaut, PERSIST=1 pour l'overlay)

## Installation

Sur la caméra (SSH root):

    # copie le tarball puis extrait
    tar xf zg2332m-plugin-$VER.tar.gz
    cd zg2332m-plugin-$VER

    # install en RAM (temporaire, effacé au reboot):
    sh install.sh

    # OU install persistant (écrit dans l'overlay jffs2):
    PERSIST=1 sh install.sh

## Commandes plugin

Accessibles via \`echo <cmd> | nc localhost 4000\`.

- **AE** : \`gainmax\`, \`expmax\`, \`profile\`, \`stockae\`, \`expinfo\`, \`route\`, \`fpn\`
- **Image** : \`blackwhite\`, \`brightness\`, \`contrast\`, \`rotation\`, \`version\`

Voir \`majestic-ae.conf\` pour l'édition des profils AE.
EOF

TAR="$OUT_DIR/zg2332m-plugin-$VER.tar.gz"
tar -C "$STAGE" -czf "$TAR" "zg2332m-plugin-$VER"
rm -rf "$STAGE"

echo "post-build-hook.sh: wrote $TAR ($(du -b "$TAR" | awk '{print $1}') bytes)"
