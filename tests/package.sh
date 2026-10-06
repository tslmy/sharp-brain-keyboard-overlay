set -eu

package=$1
expected_version=${2:-}
expected_arch=${3:-armel}
case "$expected_arch" in
    armel|armhf) ;;
    *) echo "Unsupported architecture: $expected_arch" >&2; exit 1 ;;
esac
test "$(dpkg-deb --field "$package" Architecture)" = "$expected_arch"
if [ -n "$expected_version" ]; then
    test "$(dpkg-deb --field "$package" Version)" = "$expected_version"
fi

root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
dpkg-deb --extract "$package" "$root"
binary="$root/usr/bin/keyoverlay"
test -x "$binary"
readelf --file-header "$binary" | tee "$root/elf-header"
grep -Eq "Machine:.*ARM$" "$root/elf-header"
readelf --arch-specific "$binary" | tee "$root/elf-attributes"
case "$expected_arch" in
    armel)
        grep -q "Version5 EABI, soft-float ABI" "$root/elf-header"
        grep -Eq "Tag_CPU_arch: v5TE(J)?$" "$root/elf-attributes"
        if grep -q "Tag_ABI_VFP_args: VFP registers" "$root/elf-attributes"; then
            exit 1
        fi
        readelf --program-headers "$binary" | grep -F "/lib/ld-linux.so.3"
        ;;
    armhf)
        grep -q "Version5 EABI, hard-float ABI" "$root/elf-header"
        grep -Eq "Tag_CPU_arch: v7$" "$root/elf-attributes"
        grep -q "Tag_ABI_VFP_args: VFP registers" "$root/elf-attributes"
        readelf --program-headers "$binary" | grep -F "/lib/ld-linux-armhf.so.3"
        ;;
esac