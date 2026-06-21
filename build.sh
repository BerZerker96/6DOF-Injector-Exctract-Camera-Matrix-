#!/usr/bin/env bash
set -e
x86_64-w64-mingw32-windres src/resource.rc -O coff -o r64.o
i686-w64-mingw32-windres   src/resource.rc -O coff -o r32.o
DLL="-O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ -ld3d11 -ldxgi -lpsapi -lgdi32 -luser32"
x86_64-w64-mingw32-g++ src/probe.cpp $DLL -o 6DOFProbe.dll
i686-w64-mingw32-g++   src/probe.cpp $DLL -o 6DOFProbe32.dll
GUI="-O2 -std=c++17 -static -static-libgcc -static-libstdc++ -municode -mwindows -ldwmapi -lgdi32"
x86_64-w64-mingw32-g++ src/injectgui.cpp r64.o $GUI -o 6DOFInjectGUI.exe
i686-w64-mingw32-g++   src/injectgui.cpp r32.o $GUI -DPROBE32 -o 6DOFInjectGUI32.exe
echo built
