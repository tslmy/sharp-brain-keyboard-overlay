set -eu

package=$1
expected_version=${2:-}
test "$(dpkg-deb --field "$package" Architecture)" = armel
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
grep -q "Version5 EABI, soft-float ABI" "$root/elf-header"
readelf --arch-specific "$binary" | tee "$root/elf-attributes"
grep -Eq "Tag_CPU_arch: v5TE(J)?$" "$root/elf-attributes"
if grep -q "Tag_ABI_VFP_args: VFP registers" "$root/elf-attributes"; then
    exit 1
fi
readelf --program-headers "$binary" | grep -F "/lib/ld-linux.so.3"