# Build Panfrost on your board

1. Install  dependent libraries

   ```
   sudo apt install flex bison python3-mako libwayland-egl-backend-dev libxcb-dri3-dev libxcb-dri2-0-dev libxcb-glx0-dev libx11-xcb-dev libxcb-present-dev libxcb-sync-dev libxxf86vm-dev libxshmfence-dev libxrandr-dev libwayland-dev libxdamage-dev libxext-dev libxfixes-dev x11proto-dri2-dev  x11proto-present-dev x11proto-gl-dev x11proto-xf86vidmode-dev libexpat1-dev libudev-dev gettext mesa-utils xutils-dev libpthread-stubs0-dev ninja-build bc flex bison cmake git valgrind llvm  python3-pip pkg-config zlib1g-dev wayland-protocols libxcb-shm0-dev meson
   ```

   

2. Build libdrm

   ```shell
   git clone https://gitlab.freedesktop.org/mesa/drm
   cd drm/
   mkdir build
   cd build/
   meson
   sudo ninja install
   ```

   

3. Build mesa

   ```
   git clone https://gitlab.freedesktop.org/mesa/mesa.git
   cd mesa
   mkdir build
   cd build
   meson -Dvulkan-drivers= -Dgallium-drivers=panfrost,swrast -Dlibunwind=false -Dprefix=/opt/panfrost
   sudo ninja install
   echo /opt/panfrost/lib/aarch64-linux-gnu | sudo tee /etc/ld.so.conf.d/0-panfrost.conf
   sudo ldconfig
   sudo reboot
   ```

最新的 mesa 可能对 meson 的版本有要求，如果报错提示 meson 版本过低，可以自己编译安装更高版本的 meson。
