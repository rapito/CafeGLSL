# /bin/bash
cd ..
meson build-host/ -Db_sanitize=none -Dtools=glsl -Dvulkan-drivers= -Dgallium-drivers=r600 -Db_lundef=false -Dglx=disabled -Degl=disabled -Dplatforms= -Dllvm=disabled --buildtype=debug
ninja -C ./build-host
