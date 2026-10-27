%global debug_package %{nil}
%global __brp_strip %{nil}
%global __brp_strip_comment_note %{nil}
%global __brp_strip_static_archive %{nil}
%global __brp_mangle_shebangs %{nil}
%global __brp_python_bytecompile %{nil}
%global __requires_exclude_from ^/opt/licasa/.*$
%global __provides_exclude_from ^/opt/licasa/.*$

Name:           licasa-appimage
Version:        %{licasa_version}
Release:        1%{?dist}
Summary:        Licasa image viewer, installed from its qualified AppImage
License:        BSD-2-Clause AND BSD-3-Clause AND LGPL-2.1-only AND LGPL-3.0-or-later AND MIT AND Apache-2.0
URL:            https://github.com/sanxchep/licasa
Source0:        Licasa-%{version}-x86_64.AppImage
Source1:        stage_portable_package.py
Source2:        native_lifecycle.py
BuildArch:      x86_64
BuildRequires:  python3
Requires:       bash
Requires:       python3, util-linux-core
Requires(pre): /usr/bin/python3
Requires(preun): /usr/bin/python3
Requires(post): /usr/bin/python3
Requires(postun): /usr/bin/python3
Requires:       libX11, libXext, libxcb, libxkbcommon
Requires:       mesa-libGL, mesa-libEGL, libglvnd-opengl
Requires:       fontconfig, harfbuzz, dbus-libs

%description
Licasa image viewer with its tested codecs and Qt runtime extracted at
installation time. This package runs without FUSE.

%prep

%build

%install
python3 %{SOURCE1} %{SOURCE0} %{buildroot}

# The incoming adapter runs after its ordered dependencies, before this package's
# payload is replaced. A failed preinstall skips this package's install/update.
%pre
@NATIVE_PREINSTALL@

%post
@NATIVE_COMPLETE@

%preun
@NATIVE_PREREMOVE@

%postun
@NATIVE_REMOVED@

%files
/usr/bin/licasa
/usr/share/applications/licasa.desktop
/usr/share/icons/hicolor/512x512/apps/licasa.png
%license /usr/share/licenses/licasa-appimage/LICENSE
/opt/licasa
