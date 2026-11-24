# KEEFI 의존성과 저작권 고지

KEEFI는 Fincept Terminal v4.5.1을 수정한 AGPL-3.0-or-later 프로젝트입니다. Windows x64 ZIP 안의 `KEEFI/LICENSE`와 `NOTICE`에 라이선스 전문과 원저작자 고지가 들어 있습니다. 같은 버전의 소스와 빌드 스크립트는 해당 릴리스에서 받을 수 있습니다. [KEEFI Releases](https://github.com/gurosawa/keefi-releases/releases)

## Windows 배포본

경로는 압축을 푼 `KEEFI` 폴더 기준입니다. 앱은 Qt DLL을 동적으로 로드하며 호환되는 수정본으로 교체할 수 있습니다.

| 구성 요소 | 용도와 동봉 고지 | 해당 버전의 원본 |
|---|---|---|
| Qt 6.8.3 | Core, Gui, Widgets, Network, WebSockets 및 Windows·이미지·TLS 플러그인. Copyright The Qt Company Ltd. and other contributors. [LGPLv3](licenses/qt-6.8.3-LGPL-3.0-only.txt)와 [GPLv3](licenses/qt-6.8.3-GPL-3.0-only.txt) 전문 및 [Qt 내부 구성 요소의 고지](licenses/qt-6.8.3-third-party.txt)와 공식 Windows SDK의 Qt Base·WebSockets SPDX 문서가 `licenses/`에 있습니다. | [Qt Base 6.8.3 원본](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz), [Qt WebSockets 6.8.3 원본](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtwebsockets-everywhere-src-6.8.3.tar.xz) |
| GCC 13.1.0 및 MinGW-w64 | `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`. GCC Runtime Library Exception 전문은 `licenses/gcc/COPYING.RUNTIME`에 있습니다. MinGW-w64와 winpthreads 고지는 `licenses/mingw-w64/`, `licenses/winpthreads/`에 있습니다. | [GCC 13.1.0 원본](https://ftp.gnu.org/gnu/gcc/gcc-13.1.0/gcc-13.1.0.tar.xz), [MinGW-w64 13.1.0 toolchain rt_v11-rev1](https://github.com/niXman/mingw-builds-binaries/releases/tag/13.1.0-rt_v11-rev1), [빌드 소스](https://github.com/niXman/mingw-builds/tree/v-5.0.0) |
| libsodium 1.0.22 | 로컬 금고의 암호화·키 유도. 정적 링크. [ISC 라이선스 및 저작권 고지](licenses/libsodium-1.0.22.txt) | [libsodium 1.0.22 원본](https://github.com/jedisct1/libsodium/releases/download/1.0.22-RELEASE/libsodium-1.0.22.tar.gz) |
| zlib 1.3.1 | 압축 해제. 정적 링크. 원저작자 Jean-loup Gailly와 Mark Adler의 zlib License 전문은 `licenses/zlib.h`에 있습니다. | [zlib 1.3.1 원본](https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz) |
| Pretendard 1.3.9 | 한글·본문 글꼴. Copyright 2021 Kil Hyung-jin. Reserved Font Name Pretendard. [SIL OFL 1.1](licenses/pretendard-1.3.9-OFL.txt) | [Pretendard 1.3.9](https://github.com/orioncactus/pretendard/releases/tag/v1.3.9) |
| Departure Mono 1.500 | 숫자·시각 표기 글꼴. Copyright 2022–2024 Helena Zhang. [SIL OFL 1.1](licenses/departure-mono-1.500-OFL.txt) | [Departure Mono 1.500](https://github.com/rektdeckard/departure-mono/releases/tag/v1.500) |

Windows 빌드의 SDK 파일과 SHA-256은 대응 소스의 `scripts/keefi/windows-sdk.lock.json`에 있습니다. 소스 빌드 시 CMake·Ninja·Python·Qt 빌드 도구를 사용합니다. macOS 빌드는 시스템 zlib·Iconv를 사용합니다. [Qt 6.8.3 빌드 도구 라이선스](https://github.com/qt/qttools/tree/v6.8.3/LICENSES)

## 이전 코드의 출처

이전 거래 브리지에서 사용한 `@solana/web3.js` 1.95.8의 MIT·제3자 고지를 보존합니다. 이전 Phantom AutoConfirm adapter는 MIT 라이선스의 `@phantom/browser-injected-sdk` 2.0.2 status/disable 프로토콜을 사용했습니다. [보존한 Solana 고지](licenses/solana-web3-1.95.8.txt)
