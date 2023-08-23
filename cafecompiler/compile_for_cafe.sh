#!/usr/bin/env bash
cd ..
DEVKITPRO=/opt/devkitpro ./cafecompiler/meson-cross.sh wiiu ppccross build-cafe/ -Db_sanitize=none -Dtools= -Dvulkan-drivers= -Dgallium-drivers=r600 -Db_lundef=false -Db_staticpic=false -Dglx=disabled -Degl=disabled -Dplatforms= -Dllvm=disabled --buildtype=release -Db_lto=false
ninja -C ./build-cafe
