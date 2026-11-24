# Maintainer: ric
pkgname=modern-xmb-git
pkgver=0.1.0.r0
pkgrel=1
pkgdesc="Native C++20/Vulkan/AVX2+AVX-512 recreation of the PS3 XMB dynamic silk ribbon, bokeh particles, and HDR glare engine"
arch=('x86_64')
url="https://github.com/keylimesoda/Modern-XMB"
license=('MIT')
depends=(
  'vulkan-icd-loader'
  'sdl3'
  'wayland'
  'zlib'
)
makedepends=(
  'cmake'
  'ninja'
  'vulkan-headers'
  'shaderc'
  'wayland-protocols'
  'pkgconf'
  'git'
)
provides=('modern-xmb')
conflicts=('modern-xmb')
source=("git+${url}.git")
sha256sums=('SKIP')

pkgver() {
  cd "${srcdir}/Modern-XMB"
  printf "0.1.0.r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short=7 HEAD)"
}

build() {
  cmake -B build -S "${srcdir}/Modern-XMB" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DXMB_PORTABLE=ON
  cmake --build build
}

check() {
  ctest --test-dir build --output-on-failure
}

package() {
  DESTDIR="${pkgdir}" cmake --install build
  install -Dm644 "${srcdir}/Modern-XMB/LICENSE" "${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
}
