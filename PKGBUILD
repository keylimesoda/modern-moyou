# Maintainer: ric
pkgname=modern-moyou-git
pkgver=0.1.0.r0
pkgrel=1
pkgdesc="Native C++20/Vulkan/SIMD recreation of the PS3 moyou silk ribbon, bokeh particles, seasonal sky, and HDR glare engine"
arch=('x86_64')
url="https://github.com/keylimesoda/modern-moyou"
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
provides=('modern-moyou')
conflicts=('modern-moyou')
source=("git+${url}.git")
sha256sums=('SKIP')

pkgver() {
  cd "${srcdir}/modern-moyou"
  printf "0.1.0.r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short=7 HEAD)"
}

build() {
  cmake -B build -S "${srcdir}/modern-moyou" -G Ninja \
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
  install -Dm644 "${srcdir}/modern-moyou/LICENSE" "${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
}
