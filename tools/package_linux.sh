#!/bin/sh
# Assemble the Linux release from the portable Windows release: the same
# programs, started through Wine by two launchers.
#
#     sh tools/package_linux.sh <WawConverter folder of the Windows release> [output folder]
#
# Writes <output>/WawConverter-Linux-x86_64.tar.gz and its .sha256.
set -eu
windows=${1:?usage: package_linux.sh <WawConverter folder> [output folder]}
root=$(cd "$(dirname "$0")/.." && pwd)
output=${2:-"$root/work/linux_dist"}
[ -f "$windows/B2W.exe" ] && [ -f "$windows/waw2bo2n.exe" ] || { echo "not a Windows release folder: $windows" >&2; exit 1; }

bundle="$output/WawConverter-Linux"
rm -rf "$bundle"
mkdir -p "$bundle"
cp -R "$windows/." "$bundle/"
cp "$root/docs/LINUX.md" "$bundle/START HERE.md"
cp "$root/docs/DESKTOP.md" "$bundle/Desktop guide.md"
cp "$root/tools/package_linux.sh" "$bundle/source/package_linux.sh"

# WINEPREFIX selects the prefix (Plutonium's), WAWCONVERTER_WINE the runner
launcher() {
    cat > "$bundle/$1" <<EOF
#!/bin/sh
DIR="\$(cd "\$(dirname "\$0")" && pwd)"
exec "\${WAWCONVERTER_WINE:-wine}" "\$DIR/$2" "\$@"
EOF
    chmod 755 "$bundle/$1"
}
launcher WawConverter B2W.exe
launcher WawConverter.CLI waw2bo2n.exe

archive="$output/WawConverter-Linux-x86_64.tar.gz"
tar -czf "$archive" -C "$output" WawConverter-Linux
(cd "$output" && sha256sum WawConverter-Linux-x86_64.tar.gz > WawConverter-Linux-x86_64.tar.gz.sha256)
echo "$archive"
