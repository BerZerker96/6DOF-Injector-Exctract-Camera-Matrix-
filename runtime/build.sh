#!/bin/sh
# Universal 6DOF runtime (x64). Reference build.
x86_64-w64-mingw32-g++ sixdof_runtime.cpp -O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ \
  -lws2_32 -lpsapi -o 6DOFRuntime.dll
echo "built 6DOFRuntime.dll"
