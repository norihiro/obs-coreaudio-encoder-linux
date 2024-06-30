Name: @PLUGIN_NAME_FEDORA@
Version: @VERSION@
Release: @RELEASE@%{?dist}
Summary: CoreAudio plugin for OBS Studio
License: GPLv2+

Source0: %{name}-%{version}.tar.bz2
BuildRequires: cmake, gcc, gcc-c++
BuildRequires: obs-studio-devel
Requires: wine-core

%description
CoreAudio plugin will add a CoreAudio encoder to OBS Studio.
This plugin requires DLL files retreived from iTunes.

%prep
%autosetup -p1

%build
%{cmake} -DLINUX_PORTABLE=OFF -DLINUX_RPATH=OFF
%{cmake_build}

%install
%{cmake_install}

%files
%{_libdir}/obs-plugins/@PLUGIN_NAME@.so
%{_datadir}/obs/obs-plugins/@PLUGIN_NAME@/
