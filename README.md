# OBS CoreAudio Encoder Plugin for Linux

## Introduction

This plugin provides the proprietary CoreAudio AAC encoder for OBS Studio on Linux.

This plugin relies on [Wine](https://www.winehq.org/) to run the encoder.

## Build and install

### Encoder process

At first, build the encoder process code using mingw32. You may use Visual Studio.
```sh
cd encoder-proc
mingw32-cmake -B build -D LIBOBS_INC_DIRS=/usr/include/obs
```
`LIBOBS_INC_DIRS` is required for the inline implementation of `dstr`.

### Plugin

Then, build the main plugin. Below is an example for Fedora.
```sh
mkdir build && cd build
cmake \
  -D CMAKE_INSTALL_PREFIX=/usr \
  -D CMAKE_INSTALL_LIBDIR=/usr/lib64 \
  -D ENV_WINEPATH:STRING=/usr/i686-w64-mingw32/sys-root/mingw/bin/ \
  -D WINE_EXE_PATH:STRING=/usr/bin/wine \
  ..
make
sudo make install
```
You might need to adjust `CMAKE_INSTALL_LIBDIR` and `ENV_WINEPATH` for your system.

### Install
In addition to installing by `make install`,
also copy `obs-coreaudio-encoder-proc.exe` to the data directory of the plugin.

You also need to place `CoreAudioToolbox.dll` and its depending DLL files to a proper location.
