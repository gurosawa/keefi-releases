# macOS용 Qt 6.8.3 접근성 보완

Intel 배포본은 Qt 6.8.3 공식 SDK와 Qt Base의 접근성 보완을 적용한 QtGui 및 Cocoa 플러그인을 사용합니다. 이 디렉터리의 패치는 공식 6.8.3 소스와 배포본에 사용한 두 구성 요소의 전체 차이입니다. [Qt Base 원본](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz)

Qt Base 소스 루트에서 `patch -p1 < /path/to/qtbase-6.8.3-macos-accessibility.patch`를 실행합니다. 패치를 적용한 Qt Base의 빌드 디렉터리를 아래와 같이 구성합니다. 이 예시는 Intel Mac과 macOS SDK 15.4를 사용합니다.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
  -DCMAKE_OSX_SYSROOT=/path/to/MacOSX15.4.sdk -DQT_FORCE_WARN_APPLE_SDK_AND_XCODE_CHECK=ON \
  -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DFEATURE_pkg_config=OFF \
  -DFEATURE_zstd=OFF -DFEATURE_force_debug_info=ON -DFEATURE_separate_debug_info=ON \
  -DFEATURE_reduce_relocations=OFF -DFEATURE_intelcet=OFF \
  -DOPENSSL_ROOT_DIR=/path/to/openssl-3 -DFEATURE_openssl=ON \
  -DFEATURE_openssl_linked=OFF -DFEATURE_openssl_runtime=ON
cmake --build build --target QCocoaIntegrationPlugin
```

KEEFI의 production `KEEFI` CMake 타깃을 Release로 빌드한 뒤, 저장소 루트에서 아래 스크립트를 실행합니다. `--qt-prefix`에는 [의존성 목록](../../../scripts/keefi/dependencies.json)에 지정된 공식 Qt 6.8.3 macOS SDK를 사용합니다. 스크립트는 프레임워크, 플러그인, 저작권 고지를 묶고 Intel 바이너리를 ad-hoc 코드 서명한 후, ZIP을 다시 풀어 빈 프로필에서 시작 화면을 검증합니다.

```sh
python3 scripts/keefi/package-readonly-macos.py \
  --raw-bundle /path/to/keefi-build/.app-staging.noindex/KEEFI.app \
  --qt-prefix /path/to/Qt/6.8.3/macos \
  --patched-qt-build /path/to/patched-qtbase/build \
  --output-dir /path/to/dist
```
