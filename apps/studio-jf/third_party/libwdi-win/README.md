# wdi-simple for Windows (MinGW, x86-64)

`wdi-simple.exe` binds the WinUSB driver to the ECU's USB bootloader (`0483:df11`), so the studio can
write firmware on Windows. Windows has no in-box driver for that device. The studio ships it in
`driver\` beside `studio.exe` and runs it through the administrator prompt the first time firmware is
written (`src/comms/DfuUsbWindows.cpp`, `installDriver()`); the installer can also run it.

It is libwdi's own command-line example, cross-built from the source tarball beside it, with
Microsoft's WinUSB co-installers embedded — the same build jscope ships:

    tar xf libwdi-1.5.1.tar.bz2 && cd libwdi-1.5.1
    ./autogen.sh
    ./configure --host=x86_64-w64-mingw32 --disable-32bit --enable-64bit \
                --with-wdkdir="<extracted>/Program Files/Windows Kits/8.0" \
                --enable-examples-build --disable-debug --disable-shared
    sed -i 's|/\* #undef COINSTALLER_DIR \*/|#define COINSTALLER_DIR "wdf"|; \
            s|/\* #undef X64_DIR \*/|#define X64_DIR "x64"|' config.h
    make
    x86_64-w64-mingw32-strip examples/wdi-simple.exe

    libwdi-1.5.1.tar.bz2   git archive of tag v1.5.1, github.com/pbatard/libwdi
    sha256 811903ef1a195cb1203db92281a19a7eb299ba9d63befc9c6568a9320fc75c1f

The WDK is Microsoft's "WDK 8 redistributable components" (wdfcoinstaller.msi,
https://go.microsoft.com/fwlink/p/?LinkID=253170), extracted with `msiextract`.

    wdi-simple.exe
    sha256 91903d657a4504c2915cc8c2de7fc02cdb8b503289415bd5b92860866d976386

## Licences

libwdi and wdi-simple are LGPL-3.0-or-later (`COPYING-LGPL.txt`); the source they were built from is
the tarball here. The embedded `WdfCoInstaller01011.dll` and `winusbcoinstaller2.dll` are Microsoft
redistributable components.
