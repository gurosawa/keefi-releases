# KEEFI 검증 범위

KEEFI는 금융 조회 전용 제품이다. 구현, 합성 시험, 실제 서비스 연결, 공식 화면·원장과의 수량·평가 대사는 서로 다른 증거다. 한 단계의 성공을 다른 단계의 성공으로 확대하지 않는다.

## 과거 기준 실행 — Dev101

2026-11-14 Dev101의 고정 소스 `c61e651aaa5fca5c8fd63f2226d728dbacfd170d`에서 다음 결과를 기록했다.

| 검사 | 당시 결과 |
|---|---:|
| CTest 묶음 | 186 passed / 0 failed / 186 total |
| QtTest 합계 | 4,951 passed / 0 failed / 48 skipped |

Qt 합계에는 init/cleanup과 data row가 포함된다. 4,951개의 서로 다른 사용자 기능을 뜻하지 않으며 skipped 항목은 통과로 세지 않는다. 이 결과는 해당 소스와 실행 환경에 한정된 역사적 기준이다. 문서·리소스·소스가 바뀐 후보는 그 후보에 결속한 결과로 판단한다.

기준 환경은 Intel Mac, macOS 26.7, Qt 6.8.3이었다. QtGui와 Cocoa 플랫폼 플러그인에는 로컬 접근성 보완이 포함됐다. 공식 SDK를 준비하는 [소스 빌드 안내](G5-SOURCE-BUILD.md)의 선택 검증과 이 전체 개발 검증은 같은 범위가 아니다.

## 증거의 구분

| 구분 | 확인하는 범위 |
|---|---|
| 정적 구현 | 해당 소스 경로와 제한이 존재함. 실행 결과와는 별개 |
| 합성 시험 | 가짜 인증정보·업체 형식 응답·임시 프로필로 조회, 오류 복구, 기존 자료 보존, 잠금과 변경 요청 차단 등을 검사 |
| 실제 연결 | 특정 서비스·시장·환경·앱 버전에서 실제 응답을 받은 범위 |
| 수량·평가 대사 | 같은 시점·범위의 공식 자료와 수량·가치를 비교한 범위 |

과거 특정 계좌의 조회·대사 결과를 모든 업체·시장·현재 후보의 성공으로 일반화하지 않는다. 공개 시세 수신, 빈 프로필 시작, UI 캡처, 합성 요청 차단은 실제 금융 연결이나 수량·평가 대사의 증거를 대신하지 않는다.

## 범위와 한계

금융 주문·정정·취소·공식 주문 테스트·지갑 서명·스왑 제출은 제공하지 않는다. [조회 전용 빌드 정책](../../fincept-qt/src/keefi/BuildPolicy.h)은 항상 조회 전용이며 빌드 옵션으로 거래를 켤 수 없다.

위 자동 검사 숫자는 모든 실계좌의 최신 인증·대사, 다른 물리 PC의 실제 자료 복원, 물리 디스크 고갈·전원 차단, 실제 OS 알림·VoiceOver 전체 흐름을 검증했다는 뜻이 아니다. 확인한 플랫폼 조합은 Intel Mac/macOS 26.7이다. macOS 13은 최소 컴파일 대상이며 Apple Silicon·Windows를 포함한 다른 조합의 실행 검증으로 확대하지 않는다.

현재 규칙은 [평가 정책](VALUATION-POLICY.md), [정책 결정 기록](G7-POLICY-DECISIONS.md), [저장 형식](VAULT-FORMAT.md), [기록 이동과 복구 한계](G4-RECORD-TRANSFER.md)를 따른다. [별도 재계산기](../../scripts/keefi-recon/README.md)의 C++/Python 일치는 같은 입력과 정책의 내부 일관성 근거이며 금융사의 원본 자료 대사를 대신하지 않는다.

## 소스에 구현된 서비스별 조회 범위

아래는 서비스 등록과 조회 경로를 정적으로 확인한 범위다. 표의 각 행을 실제 계좌 인증·수량·평가 대사가 완료된 결과로 읽지 않는다. 서비스 응답과 권한은 실제 연결별로 확인해야 하며, 과거 Dev101 전체 검사 결과가 이후 모든 소스와 서비스 상태를 보증하지 않는다.

| 서비스·환경 | 구현 경로와 한계 |
|---|---|
| Binance · live | 현물·USDⓈ-M 잔고/포지션, Simple Earn Flexible·Locked 및 가입·상환·보상 내역의 조회. HMAC 인증이며 RSA·Ed25519 키, Earn 가입·상환 실행은 제공하지 않음 |
| 업비트·빗썸 · live | 현물 계좌 잔고·보유 자산. 지원하지 않거나 불완전한 조회를 완전한 빈 잔고로 바꾸지 않음 |
| OKX Global · live | 통합 계좌 잔고·포지션. 같은 검증 UID의 공유 scope와 중복 합산 경계를 적용하며 다른 지역의 지원을 추정하지 않음 |
| 토스증권·키움 REST · live | 국내주식·KRW 계좌의 현금·보유 자산. 해외 계좌 전체나 모든 금융상품의 지원을 의미하지 않음 |
| 한국투자증권·LS증권 | 모든 환경에서 신규·기존 연결 비활성. 기존 등록은 금고에 보존되며 계좌·연결에서 삭제 가능 |
| Phantom / Solana · mainnet | SOL, SPL Token·Token-2022의 기본 수량. Jupiter 키·지갑 서명 없이 읽으며 스테이킹·압축 자산·비공개 잔고·확장별 표시 수량 전체를 포함하지 않음 |
| Phantom / Hyperliquid · mainnet | 기본 DEX의 USDC Standard 잔고·포지션. 통합 계좌의 공유 USDC는 불완전 범위로 두며 HIP-3·다른 담보·Portfolio Margin 전체는 포함하지 않음 |

등록 근거는 [AccountModel](../../fincept-qt/src/keefi/AccountModel.cpp), 새 연결 화면은 [AccountDialog](../../fincept-qt/src/keefi/AccountDialog.cpp), 조회 연결은 [PortfolioController](../../fincept-qt/src/keefi/PortfolioController.cpp)다. Jupiter의 보조 등록 항목을 별도의 새 계좌 연결 지원으로 확대하지 않는다. 특수 범위는 [Earn 조회 요청](../../fincept-qt/src/keefi/BinanceEarnRequests.h), [Solana 조회](../../fincept-qt/src/keefi/WalletReadController.cpp), [Hyperliquid 조회](../../fincept-qt/src/keefi/HyperliquidReadController.cpp)에서 구분한다.

자동 시험은 금고·연결·서비스 응답을 합성 자료로 다룬다. 현재 소스의 서비스 표, 역사적 Dev101 검사 합계, 실제 금융사 자료 대사는 서로 다른 범위이며 어느 것도 나머지 둘을 대신하지 않는다.
