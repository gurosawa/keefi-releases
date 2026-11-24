# Intel Mac 조회 전용 베타 — 소스 빌드

이 절차는 **검토한 소스가 있는 저장소 루트**에서 KEEFI Dev를 빌드한다. 빌드할 소스 commit을 확인하고 공개 안내와 해당 소스 버전을 함께 확인한다. 원격 기본 브랜치를 특정 검증 릴리스와 같다고 가정하지 않는다. Qt·libsodium의 고정 버전, 다운로드 해시와 SDK 준비 명령을 따른다.

## 1. 환경 확인 — 읽기 전용

```sh
uname -m
sw_vers -productVersion
xcode-select -p
xcrun --find clang++
python3 --version
git rev-parse HEAD
git status --short
```

`x86_64`인 Intel Mac, Xcode Command Line Tools의 C++20 컴파일러, Python 3.9 이상이 필요하다. 확인한 조합은 macOS 26.7·Apple Clang 17·Python 3.14.3이다. macOS 13은 최소 컴파일 대상일 뿐 실행 검증한 OS가 아니다. 다른 PC에서 SDK 설치부터 끝까지 다시 실행한 검증은 아직 없다.

Command Line Tools가 없다면 `xcode-select --install`로 Apple 설치 창을 열고 사용자가 설치를 마친다. Python이 없거나 버전이 낮으면 [python.org의 macOS 배포](https://www.python.org/downloads/macos/)를 사용한다. 이 두 준비 작업은 아래 명령이 자동 실행하지 않는다. Python의 인증서 오류가 나면 해당 배포의 인증서 설정을 완료하며 TLS 검증을 끄지 않는다.

## 2. 빌드 도구와 Qt 설치 — 로컬 폴더 생성·공개 다운로드

저장소 밖의 **비어 있는 새 폴더**를 SDK 위치로 선택한다. 아래 예시는 `~/keefi-sdk-0.5`이며 기존 설치가 있으면 덮어쓰지 말고 그 경로와 버전을 먼저 확인한다. `KEEFI_REPO`는 현재 저장소의 절대경로를 보관한다. 아래 코드 블록은 같은 터미널에서 순서대로 실행한다.

```sh
KEEFI_REPO="$PWD"
KEEFI_SDK="$HOME/keefi-sdk-0.5"
test ! -e "$KEEFI_SDK" && mkdir -p "$KEEFI_SDK/downloads"
```

위 명령이 실패하면 다음 단계로 진행하지 않는다. 기존 폴더는 삭제·초기화하지 않는다.

```sh
python3 -m venv "$KEEFI_SDK/venv"
"$KEEFI_SDK/venv/bin/python" -m pip install \
  aqtinstall==3.3.0 cmake==3.31.6 ninja==1.11.1.4
"$KEEFI_SDK/venv/bin/python" -m aqt install-qt \
  mac desktop 6.8.3 clang_64 \
  --archives qtbase qttools --modules qtwebsockets \
  --outputdir "$KEEFI_SDK/qt" \
  --keep --archive-dest "$KEEFI_SDK/downloads"
```

`venv`는 전역 Python 패키지를 바꾸지 않는다. pip는 PyPI에서 빌드 도구를 설치한다. aqt는 Qt의 공개 저장소에서 base·tools와 별도 WebSockets 모듈만 받는다. `--keep`과 `--archive-dest`는 검증할 다운로드 파일을 보관한다. `clang_64` 패키지 안에는 두 CPU용 코드가 있을 수 있지만 KEEFI의 확인 범위는 Intel이다. aqt의 기본 upstream 체크섬 검사를 끄지 않는다. 설치 명령은 [aqt 3.3.0 문서](https://aqtinstall.readthedocs.io/en/v3.3.0/cli.html)와 실제 `--dry-run` 결과를 대조했다.

## 3. SDK 압축본 고정 해시 확인·libsodium 빌드

아래 curl은 libsodium의 고정 버전 소스만 다운로드하며 실행하지 않는다. HTTPS와 HTTPS redirect만 허용한다. 이어 저장소에 고정한 SHA-256과 **Qt 3개 + libsodium 1개**를 대조한다.

```sh
/usr/bin/curl --fail --location --proto '=https' --proto-redir '=https' \
  --output "$KEEFI_SDK/downloads/libsodium-1.0.22.tar.gz" \
  https://github.com/jedisct1/libsodium/releases/download/1.0.22-RELEASE/libsodium-1.0.22.tar.gz
(
  cd "$KEEFI_SDK/downloads" &&
  /usr/bin/shasum -a 256 -c "$KEEFI_REPO/scripts/keefi/sdk-archives.sha256"
)
```

**4개 모두 `OK`이고 종료 코드가 0일 때만** 진행한다. 누락·불일치 시 해시를 새 값으로 덮어쓰거나 검사를 건너뛰지 않는다. aqt는 설치 중 upstream 체크섬을 검증하며, 위 고정 해시 비교는 설치 후 빌드 전 확인이다. 이를 설치 전 모든 실행을 통제하는 장치로 설명하지 않는다. Python·Apple 도구·pip의 모든 간접 의존성까지 고정한 완전한 재현 빌드도 아니다.

```sh
tar -xzf "$KEEFI_SDK/downloads/libsodium-1.0.22.tar.gz" -C "$KEEFI_SDK"
(
  cd "$KEEFI_SDK/libsodium-1.0.22" &&
  ./configure --prefix="$KEEFI_SDK/sodium" --disable-shared --enable-static &&
  make -j4 &&
  make check &&
  make install
)
```

`--prefix`는 설치 위치를 새 SDK 폴더 안으로 제한한다. 정적 라이브러리를 만들고 libsodium 자체 검사를 통과한 뒤 그 폴더에 설치한다. `sudo`나 시스템 경로 변경은 필요하지 않다. 각 단계가 실패하면 다음 단계로 진행하지 않는다. [libsodium 공식 설치 안내](https://doc.libsodium.org/installation)를 따른다.

## 4. KEEFI Dev 빌드·선택 검증

```sh
cd "$KEEFI_REPO"
KEEFI_QT_PREFIX="$KEEFI_SDK/qt/6.8.3/macos" \
KEEFI_SODIUM_PREFIX="$KEEFI_SDK/sodium" \
KEEFI_CMAKE="$KEEFI_SDK/venv/bin/cmake" \
KEEFI_NINJA="$KEEFI_SDK/venv/bin/ninja" \
KEEFI_PYTHON="$KEEFI_SDK/venv/bin/python" \
/bin/sh scripts/keefi/build-readonly-macos.sh
```

이미 같은 버전 SDK가 있다면 새로 설치하지 않고 실제 경로만 지정한다. 스크립트는 CMake 대상 `KEEFI_Dev`로 KEEFI Dev 후보를 만들고 가짜 응답 선택 시험, Qt 번들, 로컬 ad-hoc 서명, 외부 SDK 의존성 제거, 빈 임시 프로필 시작을 확인한다. 제품은 항상 조회 전용이며 `KEEFI_READ_ONLY_BETA=OFF`로 거래 기능을 켤 수 없다. 이 선택 검증은 금융사의 실제 연결·수량/평가 대사와 범위가 다르다. 실패한 후보를 설치하지 않는다.

- 앱: `fincept-qt/build-keefi-g5-readonly/.app-staging.noindex/KEEFI Dev.app`
- 영수증: `fincept-qt/build-keefi-g5-readonly/readonly-verification.json`

이 명령은 개인 금고를 열거나 금융 API를 호출하지 않는다. `/Applications` 설치·앱 종료·기존 프로필 복사를 수행하지 않는다. Dev 식별자는 `xyz.beamgrove.keefi.dev`이며 일반 앱과 프로필을 분리한다. 프로필 이전은 앱의 안내에서 사용자가 직접 선택한다.

첫 실제 실행의 이전 안내는 사용자가 직접 선택한다. 새 활성 Dev 프로필은 `~/Library/Application Support/KEEFI/Profiles/Dev`다. Dev100의 `KEEFI/KEEFI Dev`와 이전 제품의 프로필은 원본을 잠그고 별도 사본에서 인증·변환·검증한 뒤 새 위치에 게시한다. 원본은 보존하며 취소·실패한 경우 옛 프로필을 새 writer로 열지 않는다. `--profile-dir`는 지정한 폴더만 검사하고 구형이면 선택 후 별도 sibling 사본으로 이전한다. 서로 다른 계정 식별 방식의 백업은 빈 프로필에서 복원을 준비하고 다음 실행 때 검증·게시한다. 새 앱·새 프로필을 별도 보관하고 백업 앱·원본 프로필로 되돌릴 수 있지만, 이전 후 새 자료는 원본에 자동으로 역복사되지 않는다. 자세한 선택과 한계는 [README](../../README.md)의 프로필 이전과 복구 절과 [저장 형식](VAULT-FORMAT.md)을 따른다.

라이선스·고지 원문과 글꼴 출처 JSON은 `keefi_notices.qrc`, 기존 글꼴6개와 두 OFL 원문은 `keefi_fonts.qrc`로 실행 파일에 포함된다. 기존 제품 초기화 경로 뒤 리소스 바이트·해시를 검사하며, 새 qrc와 참조 파일도 기존 소스 fingerprint에 들어간다. 공개 바이너리의 전체 라이선스 검토·대응 소스 제공이 완료됐다는 뜻은 아니다.

Apple Developer ID 서명·공증을 받은 사전 빌드 앱은 제공하지 않는다. macOS 실행 차단은 [Apple의 해당 안내](https://support.apple.com/ko-kr/102445)를 확인하며 Gatekeeper 전체 해제나 개인 보안 설정 일괄 변경을 설치 절차로 요구하지 않는다.

후보 검증 영수증에는 Git HEAD(소스 압축본이면 없음), 소스·검사·리소스 fingerprint, 실행 파일 SHA-256, 개별 합성 case와 실행 시각을 기록한다. 빌드 중 소스가 바뀌거나 필수 검사 결과가 누락되면 성공 영수증을 만들지 않는다. 실제 서비스 확인은 [현재 검증 상태](VERIFICATION-STATUS.md)와 구분한다.

## 갱신

검토한 새 소스 커밋과 변경 기록을 확인한 뒤 같은 절차로 후보를 다시 만든다. 수정한 소스가 있으면 `git status`로 확인하고 보존한 후 버전을 바꾼다. 무조건적인 `git reset --hard`, 자동 최신판 실행, 기존 앱 위로의 자동 교체는 이 절차에 포함하지 않는다. 빌드·가짜 응답 검증과 사용자의 실제 조회·수량/평가 대조는 구분한다.
