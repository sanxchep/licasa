#!/usr/bin/env bash
# Install an AppImage as an extracted, FUSE-free per-user desktop application.
set -euo pipefail

if [[ $# -ne 1 || ! -f $1 ]]; then
  echo "Usage: $0 Licasa-<version>-x86_64.AppImage" >&2
  exit 2
fi

appimage="$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")"
home="${HOME:?HOME is required}"
data_home="${XDG_DATA_HOME:-$home/.local/share}"
if [[ $data_home != /* ]]; then
  echo "XDG_DATA_HOME must be an absolute path" >&2
  exit 2
fi
apps="$home/.local/opt"
target="$apps/licasa-appimage"
desktop_dir="$data_home/applications"
icon_dir="$data_home/icons/hicolor/512x512/apps"
desktop="$desktop_dir/licasa-appimage.desktop"
icon="$icon_dir/licasa-appimage.png"

if [[ -e $target && ! -f $target/.licasa-appimage-install ]]; then
  echo "Refusing to replace an installation not created by this script: $target" >&2
  exit 1
fi

mkdir -p "$apps" "$desktop_dir" "$icon_dir"
work="$(mktemp -d "$apps/.licasa-appimage.XXXXXXXX")"
backup=""
committed=0
moved_new=0
cleanup() {
  if [[ $committed -eq 0 ]]; then
    if [[ $moved_new -eq 1 ]]; then
      rm -rf -- "$target"
    fi
    if [[ -n $backup ]]; then
      mv "$backup" "$target"
    fi
  fi
  rm -rf -- "$work"
}
trap cleanup EXIT

cp -- "$appimage" "$work/Licasa.AppImage"
chmod u+x "$work/Licasa.AppImage"
(cd "$work" && ./Licasa.AppImage --appimage-extract >/dev/null)
extracted="$work/squashfs-root"
source_desktop="$extracted/usr/share/applications/licasa.desktop"
source_icon="$extracted/usr/share/icons/hicolor/512x512/apps/licasa.png"
for required in "$extracted/AppRun" "$source_desktop" "$source_icon"; do
  if [[ ! -f $required ]]; then
    echo "The AppImage is missing $required" >&2
    exit 1
  fi
done
mime="$(sed -n 's/^MimeType=//p' "$source_desktop")"
if [[ -z $mime || $mime == *$'\n'* ]]; then
  echo "The AppImage has no valid desktop MIME list" >&2
  exit 1
fi

# Desktop Entry Exec values require quoted paths and escaped special characters.
exec_path="$target/AppRun"
exec_path="${exec_path//\\/\\\\}"
exec_path="${exec_path//\"/\\\"}"
exec_path="${exec_path//\$/\\\$}"
tick=$'\x60'
exec_path="${exec_path//$tick/\\$tick}"
exec_path="${exec_path//%/%%}"
printf '%s\n' 'Installed by packaging/install-licasa-appimage.sh' > "$extracted/.licasa-appimage-install"
desktop_new="$work/licasa-appimage.desktop"
cat > "$desktop_new" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=Licasa (AppImage)
Comment=Image viewer
Exec="$exec_path" %f
Terminal=false
Categories=Graphics;Viewer;
MimeType=$mime
Icon=licasa-appimage
StartupNotify=false
EOF

if [[ -e $target ]]; then
  backup="$(mktemp -d "$apps/.licasa-backup.XXXXXXXX")"
  rmdir "$backup"
  mv "$target" "$backup"
fi
mv "$extracted" "$target"
moved_new=1
cp "$target/usr/share/icons/hicolor/512x512/apps/licasa.png" "$icon"
mv "$desktop_new" "$desktop"
committed=1
if [[ -n $backup ]]; then
  rm -rf -- "$backup"
  backup=""
fi
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "$desktop_dir"
fi
echo "Installed Licasa in $target"
echo "Find 'Licasa (AppImage)' in your application menu or use Open With in your file manager."
