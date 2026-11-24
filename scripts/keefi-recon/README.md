# KEEFI 별도 재계산기 — G7 정책 v2

시각 T의 잔고·가격 묶음을 받아 [평가 정책 v2](../../docs/keefi/VALUATION-POLICY.md)로 다시 계산한다. Python 표준 라이브러리 `decimal`을 사용하고 KEEFI 바이너리·라이브러리를 호출하지 않는다. 앱의 금액과 별도 실행 구현의 금액이 같은지, 문서 규칙을 양쪽이 같은 뜻으로 구현했는지 검사한다.

## 독립성·검증 경계

- 최초 build 47판은 Claude가 정책 문서를 보고 Python으로 작성했다. 정책 문서 작성 단계에서 이미 C++를 읽었으므로 최초판도 작성자 독립은 아니었다.
- **G7 수정에서는 Astra가 현재 C++ 동작과 Python을 직접 대조했다.** “현재 구현이 C++ 코드를 전혀 보지 않고 작성됐다”는 주장은 하지 않는다. A1–A13의 차이를 문서·C++·Python에서 명시적으로 표준화했다.
- 별도 언어·실행 구현은 유지되지만 공통 정책의 오류까지 독립 검증한다고 보장하지 않는다. 재계산기 일치는 내부 일관성·정책 일치의 증거다.
- 거래소·지갑 공식 화면/원장과 실제 수량·가치를 맞추는 T2/T3/T4 대사를 대신하지 않는다. 작성자까지 독립된 검증이 필요하면 정책 문서와 입력 스키마만 받은 다른 구현을 별도로 비교한다.
- 이 폴더는 현재 앱 소스 지문에 포함된다(`scripts/keefi/verification_receipt.py`의 `scripts/keefi-recon` 범위). 같은 지문에 결속된다는 사실이 실제 금융사 자료의 정확성이나 작성자 독립을 증명하지는 않는다.

## 실행

작업 폴더에서 실행한다. 예시 파일은 합성 계좌·가격이며 네트워크나 금고를 열지 않는다.

```bash
PYTHONDONTWRITEBYTECODE=1 python3 scripts/keefi-recon/recalc.py scripts/keefi-recon/example-bundle.json
PYTHONDONTWRITEBYTECODE=1 python3 scripts/keefi-recon/test_recalc.py
```

- `--json`: 정확한 행 값·상태·환산 다리·시각을 JSON으로 출력한다.
- `--as-of <ms>`: 평가 시각을 명시한다.
- `--recorded`: 저장 계산 규칙으로 평가한다. 앱의 동일 실행·검증 scope 표시 허가까지 검사하지는 않는다.
- `python3 -I`로 시험을 실행하면 같은 폴더의 `recalc`를 찾지 못한다. 위 명령은 프로젝트 파일을 가져오는 실행 방식이다.
- G7 정책 시험에는 입력 순열, 원본 시각·사적 quote, 미래/누락 다리, 정확한 24시간 경계, Solana 버림 순서, 통화·PnL·중복·제외·저장 scope가 포함된다. 무작위 합성 포트폴리오 300개도 같은 합계·순열 성질을 검사한다. 최신 개수와 실행 결과는 실제 시험 출력으로 확인한다.

### 실제 내보낸 묶음과 비교

```bash
PYTHONDONTWRITEBYTECODE=1 python3 scripts/keefi-recon/compare.py \
  outputs/recon/T2/keefi/keefi-bundle.json \
  --output outputs/recon/T2/report/keefi-vs-recalc.json
```

경로는 사용자가 실제로 저장한 묶음으로 바꾼다. 저장 화면·로딩이 끝난 뒤 현재 평가를 내보내야 한다. `--output`은 새 파일만 만들며 권한은 0600이다. 기존 파일은 덮어쓰지 않는다. 종료 코드는 0 일치, 1 차이, 2 입력·출력 오류다. 표준 출력은 검사·차이 개수만 내고, 금액·수량 차이는 비공개 결과 파일에 담는다.

비교 범위는 `keefi_result`의 합계·화면 제목/값·그룹 순서/수량/값/비중·행 상태/값/손익·제외 여부·소액/미평가 필터 대상이다. 같은 ID의 중복 행도 순번별로 보존한다. 필수 필드가 빠진 입력을 일치로 통과시키지 않는다. 가격 근거 URL/각 다리의 내용, 원시 응답의 진위, `provider_reconciliations`, 거래소 화면과의 수량·가치 대사는 이 도구가 검증하지 않는다. `MATCH`는 **내보낸 입력에 대한 C++와 Python의 일치**이며 실계좌 대사 완료가 아니다.

추가 시험: `PYTHONDONTWRITEBYTECODE=1 python3 scripts/keefi-recon/test_compare.py`. 앱의 실제 C++ 평가·직렬화 결과를 임시 파일로 내보내 Python 프로세스와 비교하는 합성 통합 시험은 `tst_keefi_valuation_bundle`에 있다.

## 입력 `keefi-valuation-bundle/1`

최상위에는 `format`, `as_of_ms`, `connections`, `snapshots`, `quotes`, `reference_quotes`가 있다. 추가 선택 필드는 `loading`과 `excluded_assets`다. 모르는 진단 필드는 무시한다.

- 금액·수량·가격은 정수 1~80자리, 소수 0~80자리의 문자열이다. 0과 미확인은 구분한다.
- 연결·스냅숏의 environment가 live/mainnet인 범위만 평가한다.
- market은 spot/usdm_futures/domestic_equity/solana, component kind는 Holding/Cash/Liability/Equity/BuyingPower/Withdrawable/PositionNotional/UnrealizedPnl/ProviderPnl이다.
- 스냅숏에는 account_scope/connection_id/provider/environment/market/time_ms/balance_complete/evidence/components가 있다. 행 evidence가 없으면 스냅숏 evidence를 상속한다.
- 행의 선택 필드는 reported_value/reported_value_currency/provider_unrealized_pnl/provider_pnl_currency/evidence다. 보고 금액이 있으면 통화를 명시해야 한다. 빈 통화를 KRW로 추정하지 않는다.
- Solana 토큰 식별은 `solana:<Mint>`다. excluded_assets에는 이 canonical 식별의 문자열 목록을 넣는다. 유효한 32바이트 base58 Mint만 제외 대상으로 인정하고 심볼·이름·invalid 주소는 무시한다.
- quote는 base/quote/price/venue/evidence다. venue는 upbit/binance/jupiter/account 중 하나를 명시하고 실제 evidence scope도 보존한다. 사적 account quote는 공통 가격 계산에 쓰지 않는다.
- 공개 교차 다리의 exact scope는 `public:upbit:KRW-<자산>`, `public:binance:<자산>USDT`, `public:jupiter:price:<Mint>`다. 예시 파일도 같은 canonical scope를 쓴다.
- evidence의 source/url/scope/source_time_ms/received_at_ms/complete를 보존한다. 원본 시각 0은 미제공이며 수신 시각으로 덮어쓰지 않는다.
- build 52는 공개 가격의 원본이 수신보다 최대 2,000ms 앞서는 서버·PC 시계 차이를 허용한다. 원본이 평가 시각보다 미래인 경우는 계속 거절한다. 원본·수신 시각은 그대로 유지하며 현재 180초·참고 24시간 유효시간도 바꾸지 않는다. 과거 build 51의 export는 이 결함 수정 때문에 재계산 결과가 달라질 수 있으므로 원래 결과를 덮어쓰지 않는다.
- reference_quotes는 현재 값에 쓰지 않는 오래된 공개 가격이다. 최대 24시간 이내의 자격 있는 직접·교차 가격만 참고로 표시하며 합계에서 제외한다.

## 출력 `keefi-valuation-result/1`

| 부분 | 의미 |
|---|---|
| valuation | complete, net_assets, known_subtotal, 누락 사유. 표현 불가능한 합계는 null이며 0이 아님 |
| screen | 제목·원 단위 표시값·완전 여부·참고 합계·미평가/참고/사용자 제외 그룹 수 |
| groups/rows | identity·보유 수량·정확한 평가액·비중과 Fresh/Reference/NotCounted/Unvalued 상태, price route·다리 |
| rows.excluded_by_user | 명시적 평가 범위 제외. 원 수량은 보존하고 가격·0원 평가를 만들어 넣지 않음 |
| allocation/dust/hide_unvalued | 비중, 1 USDT 소액 숨김 기준·H·D, 미평가 목록 필터 대상 |
| policy_decisions | 확정한 A1–A13 규칙의 요약 |
| assumptions | 이전 소비자와의 필드 호환용 빈 객체. 확정한 규칙을 미정 가정으로 남기지 않음 |

사용자 제외 그룹이 있으면 제목은 완전할 때 `평가 대상 합계`, 미완료일 때 `평가 대상 부분합`이다. 저장 계산 결과는 `마지막 확인 평가 대상 합계`다. 일반 NotCounted는 그룹 수량에서 빼지만 사용자 제외 행은 수량에 남긴다.

Dev 84부터 내보낸 파일은 `screen_presentation: 2`를 기록한다. 확인된 금액만 있으면 `확인된 자산 부분합`, 가격이 하나도 없으면 `—`, 확인된 금액이 0원이면 `0`을 표시한다. 이 필드가 없는 과거 파일은 당시 화면 문구로 대조하며, 두 버전의 평가 계산식은 같다.

## A1–A13 결정과 구현 추적

2026-10-21 확정한 정책을 반영했다. 권위 있는 규칙은 [평가 정책 v2](../../docs/keefi/VALUATION-POLICY.md), 변경 전 C++/Python 차이와 결정 기록은 [G7-POLICY-DECISIONS.md](../../docs/keefi/G7-POLICY-DECISIONS.md)다.

| ID | 결정 | 구현·시험 위치 |
|---|---|---|
| A1 | 공개 exact scope·출처·URL·각 다리 시각 독립 검증 | public_quote/leg/cross, G7Policy invalid-leg 시험 |
| A2 | MintUSD × USDC-KRW를 먼저 곱하고 ÷ USDCUSD를 24자리에서 버림 | cross, G7Policy exact-division 시험 |
| A3 | 3개 또는 4개 실제 다리의 최소 원본·수신 시각 | cross, 기존 thin-Upbit/legs 시험 |
| A4 | 최신 원본(없으면 수신) 기준 충돌, 독립 교차·참고 시도 | current_price/convert, 순열·충돌 시험 |
| A5 | 참고 직접 충돌 뒤 독립 교차 참고 허용, 최대24h | reference_price, 참고 충돌·24h 시험 |
| A6 | 80자리 한도, 합계 초과는 null·유효 개별 행 유지 | value/bounded_add, overflow 시험 |
| A7 | 보고 통화 명시와 실제 공개 환산 경로 요구 | value/convert, 통화 누락·USD 시험 |
| A8 | 손익 complete/source/scope·통화·최신시각 요구 | provider_pnl, 미완료 PnL 시험 |
| A9 | 유효 비0 보유 우선, 불확실0/선물0 별도 확인 | presence/value, 보유·0 근거 시험 |
| A10 | 최신 평가액 순/기타/확인0, identity 동률 | groups_of, 기존 그룹 정렬 시험 |
| A11 | 사적 quote를 다른 계좌의 공통 가격으로 사용하지 않음 | public_quote, private quote 시험 |
| A12 | 중복 scope 뒤 항목 제외, 중복행 미평가 보존, 중복equity 차단 | value, duplicate 시험 |
| A13 | 관련 저장 scope 선별·연결 state 재구성·미래 검사 | recorded, 저장 scope/state 시험 |

## 합성 예시

example-bundle.json은 업비트·Binance 현물·선물·Phantom·KIS의 가짜 입력이다. 실제 계좌가 아니다.

- 원 단위 합계는 7,102,153 KRW이며 판정은 완전하다.
- G7 A2 순서의 정확한 부분합은 `7102153.30102773165974896234435`다. 이전 ratio-first 예시와 끝자리가 달라도 1원 반올림 결과는 같다.
- SOL·정식 USDC는 거래소/지갑 identity가 나뉜다. ZEC는 Binance 교차, 소액 두 그룹은 1 USDT 기준으로 숨김 대상이다.
- 이 결과는 실계좌 대사·실시간 시세 확인·설치본 검증 결과가 아니다.
