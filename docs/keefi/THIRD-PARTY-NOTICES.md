# KEEFI 소스 베타의 의존성과 고지

이 고지는 `KEEFI_G1_MODE=ON`, `KEEFI_READ_ONLY_BETA=ON`의 C++/Qt target과 필요한 빌드·검사 도구를 기준으로 한다. 소스 제공 범위를 설명하며 SDK나 사전 빌드 앱을 함께 재배포한다는 뜻이 아니다.

| 구성 요소 | 버전·사용 범위 | 라이선스·원문 |
|---|---|---|
| Fincept Terminal | v4.5.1 기반 소스와 KEEFI 수정 | AGPL-3.0-or-later. [최신 적용 안내](LICENSING.md), [전체 라이선스](../../LICENSE) |
| Qt | 6.8.3, Core·Gui·Widgets·Network·WebSockets, 패키징에 포함된 DBus와 platform/image/TLS 플러그인. Test는 시험용 | 라이브러리의 open-source 선택지는 LGPL-3.0-only 등. [Qt v6.8.3 원문](https://github.com/qt/qtbase/tree/v6.8.3/LICENSES), [WebSockets 원문](https://github.com/qt/qtwebsockets/tree/v6.8.3/LICENSES). Copyright The Qt Company Ltd. and other contributors |
| Qt 도구 | 6.8.3, moc·rcc·macdeployqt 등 빌드 도구 | 도구별 GPL 및 Qt 예외 등. [qttools v6.8.3](https://github.com/qt/qttools/tree/v6.8.3/LICENSES). 라이브러리와 도구의 조건을 동일하다고 단정하지 않음 |
| libsodium | 1.0.22, 로컬 암호화·키 유도·서명 확인, 정적 링크 | ISC. Frank Denis, 2013–2026. [동봉 원문](licenses/libsodium-1.0.22.txt), [고정 소스 출처](../../scripts/keefi/dependencies.json) |
| zlib·Iconv | KEEFI의 압축 해제·문자 변환. 현재 Intel macOS에서는 시스템 제공 라이브러리 | macOS SDK/system library 사용. 별도 사본을 KEEFI 저장소에 넣지 않음. 다른 플랫폼으로 확장할 때 해당 공급본의 조건 확인 |
| Pretendard | v1.3.9, 공식 릴리스의 정적 OTF Regular·Medium·SemiBold·Bold·ExtraBold. 한글·본문·제목 | SIL Open Font License 1.1. Copyright (c) 2021, Kil Hyung-jin; Reserved Font Name Pretendard. [동봉 원문](licenses/pretendard-1.3.9-OFL.txt), [공식 릴리스](https://github.com/orioncactus/pretendard/releases/tag/v1.3.9), [파일 SHA-256·출처](../../fincept-qt/resources/keefi/fonts/sources.json) |
| Departure Mono | v1.500, 공식 릴리스의 Regular OTF. 가격·숫자·시각·코드·라벨 | SIL Open Font License 1.1. Copyright 2022–2024 Helena Zhang. [동봉 원문](licenses/departure-mono-1.500-OFL.txt), [공식 릴리스](https://github.com/rektdeckard/departure-mono/releases/tag/v1.500), [파일 SHA-256·출처](../../fincept-qt/resources/keefi/fonts/sources.json) |
| 빌드 보조 도구 | aqtinstall 3.3.0·CMake 3.31.6·Ninja 1.11.1.4 | 빌드 시 별도 설치하며 KEEFI 앱에 넣지 않음. 공급 패키지의 고지 유지 |

## 과거 코드의 출처 고지

다음 고지는 이전 코드의 출처를 보존한다. 현재 KEEFI C++/Qt target에는 해당 거래 브리지와 AutoConfirm adapter를 연결하지 않는다.

| 구성 요소 | 과거 사용 범위 | 보존하는 고지 |
|---|---|---|
| `@solana/web3.js` | 1.95.8 vendored IIFE, 이전 거래 브리지의 역직렬화 코드 | MIT 및 해당 번들의 제3자 고지. [동봉 원문](licenses/solana-web3-1.95.8.txt) |
| Phantom AutoConfirm adapter | `@phantom/browser-injected-sdk` 2.0.2의 status/disable 프로토콜 | 기존 고지의 라이선스는 MIT. 해당 adapter 리소스는 현재 트리에 포함하지 않음 |

## Qt의 정확한 버전과 간접 구성 요소

설치된 공식 Qt 6.8.3 SDK의 `sbom/qtbase-6.8.3.spdx.json`, `qtwebsockets-6.8.3.spdx.json`, `qttools-6.8.3.spdx.json`을 확인했다. 최신 웹 문서의 6.8 패치는 6.8.3과 다를 수 있으므로 그 목록을 이번 바이너리의 구성 목록으로 복사하지 않는다. 예를 들어 설치된 SDK의 PCRE2는 10.45, FreeType은 2.13.3으로 기록돼 있다.

Qt 내부에는 BSD·MIT·zlib·기타 조건의 코드가 함께 있다. [Qt의 제3자 고지 안내](https://doc.qt.io/qt-6.8/licenses-used-in-qt.html)와 정확한 버전의 SBOM·소스를 기준으로 취급한다. 위 표는 KEEFI target의 의존성 안내이며 전체 원본 저장소나 미래 설치 파일에 대한 완전한 SBOM·바이너리 배포 검토를 주장하지 않는다.

## 소스 제공과 향후 바이너리

KEEFI 수정 소스·빌드 스크립트·고정 의존성 출처·기존 저작권 고지를 같이 제공한다. Qt SDK의 전체 소스와 라이선스는 [Qt 6.8.3 공식 소스](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/)에서 확인할 수 있다. 라이브러리 교체·수정·재빌드를 막는 별도 KEEFI 사용 조건을 추가하지 않는다.

바이너리를 게시하기로 범위를 바꾸면 실제 포함 파일을 기준으로 라이선스 원문·간접 고지·대응 소스와 재빌드 정보를 함께 준비한다. 현재 소스 베타에 대해 이를 이미 완료한 바이너리 배포로 표시하지 않는다. 설치 SDK의 개별 파일을 재배포할 때도 이 표 하나로 필요한 고지를 대체하지 않는다.
