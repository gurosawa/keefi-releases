# KEEFI

KEEFI는 한국어 개인 PC용 통합 자산 조회 데스크톱 앱입니다. Fincept Terminal v4.5.1의 Qt/C++ 소스를 기반으로 하며, 제품 이름은 **KEEFI**, 개발 앱 이름은 **KEEFI Dev**입니다.

**금융 조회 전용입니다.** 주문·정정·취소·공식 주문 테스트·지갑 서명·스왑 제출을 제공하지 않습니다. 설정이나 `KEEFI_READ_ONLY_BETA=OFF`로 거래 기능을 켤 수 없습니다. 주문 이력 조회와 과거 기록의 읽기 호환은 해당 구현 범위에서 유지합니다.

Windows x64 테스트 배포본은 [GitHub Releases](https://github.com/gurosawa/keefi-releases/releases)에서 받습니다. 해당 버전의 자산에서 Windows ZIP을 내려받아 폴더 전체를 압축 해제하고 `KEEFI.exe`를 실행하세요. 개발 도구 설치나 저장소 권한은 필요하지 않습니다. 서명되지 않은 후보이므로 PC의 보안 정책에 따라 실행이 차단될 수 있습니다.

키움 REST 테스트는 국내주식 차트의 **증권 연결하기**에서 키움을 선택해 진행하세요. 앱에서 직접 두 키 파일을 불러오고 연결한 뒤, 보던 종목과 시간 간격으로 돌아오는지, 자산에 표시된 현금과 보유 수량이 증권사 화면과 일치하는지 확인할 수 있습니다. 금융 정보를 공개 이슈에 첨부하지 마세요.

Windows x64 소스 빌드와 검증은 PowerShell에서 `./scripts/keefi/build-windows-candidate.ps1 -WorkDir "$env:TEMP/keefi-build"`로 실행합니다. SDK 다운로드, SHA-256 확인, Release 빌드와 테스트가 끝나면 지정한 폴더의 `dist`에 결과가 생성됩니다.

## 제공하는 기능

- 연결 계좌의 자산을 모아 보고 보유 자산 필터와 검색을 사용할 수 있습니다. 화면 필터는 원본 수량과 평가 합계를 바꾸지 않습니다.
- 현재 원화 평가, 오래된 가격의 참고 평가, 미평가를 구분합니다. 미평가를 0으로 채우거나 USDT·USDC를 USD 1로 가정하지 않으며 참고 평가를 현재 합계에 더하지 않습니다.
- 공개 시세·차트·관심 목록을 제공합니다. 출처·시각과 자료가 부족한 구간을 구분하며, 시장 정보 화면의 값이 모든 자산의 평가 근거가 되는 것은 아닙니다.
- 자산 기록을 암호화해 보관하고 선택한 기록을 암호화 패키지로 내보내거나 가져올 수 있습니다. 이 기능은 금융 키·금고·시드를 옮기거나 현재 계좌와 자동 병합하지 않습니다. [기록 이동과 복구 한계](docs/keefi/G4-RECORD-TRANSFER.md)를 확인하세요.

금고 암호 규칙, 합계에서 빠지는 항목, 서비스별 키 입력값처럼 화면의 각 항목이 뜻하는 바는 [사용 안내](docs/keefi/USER-GUIDE.md)에 정리했습니다.

## 서비스별 조회 범위

아래는 소스에 구현된 연결·조회 범위입니다. 모든 금융사의 실제 계좌 인증·수량·평가액 대사가 끝났다는 표가 아닙니다. 근거와 검증 종류는 [검증 상태](docs/keefi/VERIFICATION-STATUS.md)에 구분했습니다.

| 서비스 | 구현된 조회 범위 | 주요 제한 |
|---|---|---|
| Binance | 현물, USDⓈ-M 선물 잔고·포지션, Simple Earn Flexible·Locked와 관련 내역 조회 | `live`, HMAC 키. RSA·Ed25519 키와 Earn 가입·상환 실행은 제공하지 않음 |
| 업비트·빗썸 | 현물 계좌 잔고·보유 자산 | `live`; 조회 가능한 범위와 가격 자료가 모두 같다고 가정하지 않음 |
| OKX | Global 지역의 통합 계좌 잔고·포지션 | `live`; 다른 지역의 지원이나 동일 자산 중복 합산을 추정하지 않음 |
| 토스증권·키움 REST | 국내주식 계좌의 현금·보유 자산 | `live`·KRW. 해외 계좌 전체나 모든 금융상품 지원을 뜻하지 않음 |
| 한국투자증권·LS증권 | 연결 비활성 | 기존 등록은 계좌·연결에서 삭제만 가능. 자산 조회·평가에서 제외 |
| Phantom의 Solana 조회용 주소 | 메인넷 SOL, SPL Token·Token-2022 기본 수량 | 지갑 서명 없이 조회. 스테이킹·압축 자산·비공개 잔고·확장별 표시 수량 전체를 포함하지 않음 |
| Phantom의 Hyperliquid 조회용 주소 | 기본 DEX의 USDC Standard 잔고·포지션 | 통합 계좌의 공유 USDC는 불완전 범위로 표시. HIP-3·다른 담보·Portfolio Margin 전체 지원을 뜻하지 않음 |

새 연결은 서비스별 실전(`live`) 또는 메인넷(`mainnet`)만 허용합니다. 예전 `paper`·`demo`·`testnet` 연결은 읽기·재저장·삭제 호환을 유지하지만 조회·평가하거나 실전으로 자동 전환하지 않습니다. [금고 호환 규칙](docs/keefi/VAULT-FORMAT.md)을 따릅니다.

## 금융 키와 데이터

연결에는 **조회에 필요한 권한만 있는 키**를 사용하세요. 주문·출금 권한을 부여하지 마세요. 실제 계좌 연결과 금고 암호 입력은 앱에서 직접 진행합니다. 키·Secret·토큰·암호·시드·원본 인증 응답·잔고나 계좌 화면을 이슈·채팅·이메일에 보내지 마세요.

금융 키는 이 PC의 금고에 보관합니다. 금고는 salt를 사용한 **Argon2id** 키 유도와 **XChaCha20-Poly1305** 인증 암호화를 사용합니다. 별도 운영자 가입이나 중앙 계정·클라우드 동기화를 사용하지 않으며, 인증·조회 요청은 선택한 금융사·시세·블록체인 서비스로 직접 전송합니다.

첫 금고 해제에서는 현재 잔고·가격 조회를 기다리는 로딩 화면을 표시합니다. 디스크의 저장 값을 현재 자료처럼 먼저 보여주지 않습니다. 같은 실행에서 다시 해제할 때는 직전에 검증한 연결 범위가 같고 저장 평가가 완전한 경우에만 확인 시각을 붙인 `마지막 확인` 화면을 사용하며 갱신합니다. 15초는 지연 안내 기준이며 성공이나 완료 판정이 아닙니다. [평가 정책](docs/keefi/VALUATION-POLICY.md)에 자세한 조건이 있습니다.

인증 실패·만료 뒤에는 계좌·연결에서 직접 다시 연결합니다. OS 잠금·절전 뒤 금고를 자동으로 다시 열지 않습니다. 암호화 저장은 잠금 해제 중의 화면과 운영체제 전체를 보호한다는 보장이 아니며, Qt의 모든 임시 메모리 사본까지 완전 소거한다고 주장하지 않습니다. 조회 자료의 누락·지연과 저장 실패를 구분해서 확인해야 합니다.

## 화면 표기

무기한 선물은 시세 이름의 **`.P`** 접미사로 구분합니다. 별도 `Spot`·`현물 USDT` 표시는 붙이지 않으며, 시세 출처와 설명은 `ⓘ`에서 확인합니다. 기간 비교에 필요한 자료가 부족하면 해당 비교를 사용할 수 없다고 표시합니다. 자산 평가액의 변화만으로 입출금을 제외한 투자수익률을 확정하지 않습니다.

## 검증과 지원 환경

확인된 과거 실행 기준은 **Intel Mac / macOS 26.7 / Qt 6.8.3의 Dev101**입니다. 당시 전체 개발 검사 결과는 CTest **186/186**, QtTest **4,951 passed / 0 failed / 48 skipped**였습니다. Qt 합계에는 init/cleanup과 data row가 포함되며 48개 skipped는 통과가 아닙니다.

이 기준의 QtGui·Cocoa에는 로컬 접근성 보완이 포함됐습니다. 공식 Qt SDK를 이용하는 소스 빌드 안내의 선택 검증과 같은 범위가 아니며, 이후 후보의 결과나 다른 PC·실계좌 검증으로 확대하지 않습니다. macOS 13.0은 최소 컴파일 대상입니다. macOS 13 실행, Apple Silicon·Windows의 동등한 동작, 새 물리 PC의 전체 복원은 확인된 범위가 아닙니다. [검증 상태와 한계](docs/keefi/VERIFICATION-STATUS.md)를 먼저 확인하세요.

## Intel Mac 소스 빌드

[SDK 준비부터 시작하는 빌드 안내](docs/keefi/G5-SOURCE-BUILD.md)를 따르세요. Xcode Command Line Tools의 C++20 컴파일러, Qt **6.8.3**의 Core·Gui·Widgets·Network·WebSockets·Test와 `macdeployqt`, libsodium **1.0.22**, CMake·Ninja·Python 3.9 이상이 필요합니다. 다운로드 출처와 고정 SHA-256은 [의존성 기록](scripts/keefi/dependencies.json)에서 확인할 수 있습니다.

아래 명령은 **이미 준비한 SDK**를 사용합니다. 저장소 루트에서 실제 경로로 바꿔 실행합니다.

```sh
KEEFI_QT_PREFIX=/path/to/Qt/6.8.3/macos \
KEEFI_SODIUM_PREFIX=/path/to/libsodium-1.0.22 \
KEEFI_CMAKE=/path/to/cmake \
KEEFI_NINJA=/path/to/ninja \
KEEFI_PYTHON=/path/to/python3 \
/bin/sh scripts/keefi/build-readonly-macos.sh
```

스크립트는 KEEFI Dev를 빌드하고 합성 응답 선택 검사, Qt 번들, 로컬 ad-hoc 서명, 외부 SDK 의존성 검사와 빈 임시 프로필 시작을 수행합니다. 결과 앱은 `fincept-qt/build-keefi-g5-readonly/.app-staging.noindex/KEEFI Dev.app`입니다. 이는 Apple Developer ID 서명·공증이나 금융사의 실제 계좌 검증을 뜻하지 않습니다. SDK 다운로드부터 다른 PC에서 모두 재현됐다는 보장도 아닙니다.

### 프로필 이전과 복구

소스 빌드는 앱 설치·종료·개인 프로필 이전을 자동 실행하지 않습니다. Dev와 일반 앱의 프로필은 분리됩니다. 새 Dev 활성 프로필은 `~/Library/Application Support/KEEFI/Profiles/Dev`입니다.

구형 프로필 이전은 앱의 안내에서 직접 선택합니다. 원본을 보존하고 별도 사본에서 인증·변환·검증한 뒤 새 위치를 사용합니다. 취소·실패한 원본을 새 writer로 열지 않습니다. `--profile-dir`는 지정한 폴더만 검사합니다. 이전 뒤 새 프로필의 변경은 원본에 자동 병합되지 않습니다. 파일명·형식·백업 복원의 자세한 경계는 [현재 저장 계약](docs/keefi/VAULT-FORMAT.md)을 따릅니다.

공개 시세 수집기는 `keefi-collector`, 기본 자료 위치는 `~/Library/Application Support/KEEFI-Collector/v1`입니다. 기존 수집 프로세스나 자료의 자동 이전은 포함하지 않습니다.

정식 앱이 공개되면 **Releases**에서 변경 내역과 지원 Mac을 확인하고 새 버전을 직접 내려받아 설치합니다. 소스로 설치한 경우에는 원하는 버전의 태그에서 다시 빌드할 수 있습니다.

## 라이선스와 원본

KEEFI는 **AGPL-3.0-or-later**를 따르는 독립적인 파생 제품입니다. Fincept의 공식 배포나 보증으로 표시하지 않습니다. upstream 기준은 `0aa46db2a06b8d8d9f91f07090aa214ac02db5c2`이며 [원본 README](docs/keefi/upstream/README.md)를 보존합니다.

[LICENSE](LICENSE), [저작권·수정 고지](NOTICE), [라이선스 적용 설명](docs/keefi/LICENSING.md), [의존성 고지](docs/keefi/THIRD-PARTY-NOTICES.md)를 함께 확인하세요. Fincept와 기존 FINIX 기여자의 원저작권, AGPL·MIT·OFL 등 각 구성 요소의 원문 고지를 유지합니다. 제품에 고지 리소스를 포함한 사실만으로 향후 바이너리 배포의 전체 고지·대응 소스 검토가 끝났다고 주장하지 않습니다.

## 문제 신고

일반 버그는 [버그 양식](.github/ISSUE_TEMPLATE/bug_report.md)에 앱 버전·운영체제·재현 절차만 적어 신고하세요. 보안 문제는 공개 이슈에 상세 내용을 올리지 말고 [보안 정책](SECURITY.md)의 GitHub 비공개 취약점 신고 안내를 따르세요.
