# G7 평가 정책 v2 — 정책 결정과 구현 기준

작성: 2026-10-21. 변경 전 조사 기준: build 50 소스, HEAD `3a0bd5f7`. 아래는 정책 v2의 선택과 변경 전 비교 근거다. 현재 규칙은 [평가 정책](VALUATION-POLICY.md), 실행 증거의 범위는 [검증 상태](VERIFICATION-STATUS.md)에서 구분한다.

## 적용 범위와 검증 기준

가격·계산, 표시·유효시간, 가격 없는 자산의 평가 범위를 세 묶음으로 기록한다. 채택한 선택과 날짜, 코드·문서·재계산기의 대응을 함께 보존한다. 현재 동작을 명세화한 선택과 동작을 바꾼 선택을 구분한다.

1. 가격·계산 규칙을 확정한다. 고정 ID는 표 1·2, A1–A13을 계속 사용한다.
2. 표시·유효시간 규칙을 확정한다. 역사적 build 기록을 고쳐 과거에 구현됐다고 만들지 않고, 정책 v2에서 현재 적용 규칙과 적용 build를 적는다.
3. 가격 없는 자산의 평가 범위를 확정한다. 목록 숨김과 평가 대상 제외를 다른 동작으로 유지한다.
4. 같은 합성 입력을 C++와 Python 양쪽에서 평가하고 정확한 금액·행 상태·부분합·완전 판정을 대조한다. 화면의 원 단위 반올림 일치만으로 완료하지 않는다.
5. [평가 정책](VALUATION-POLICY.md), [재계산기](../../scripts/keefi-recon/README.md), 실제 코드·시험을 한 변경 묶음으로 맞춘다. 결정된 항목은 `ASSUMPTIONS`에 미정인 것처럼 남기지 않는다.

아래 비교는 변경 전 소스의 차이를 기록한다. 각 항목에 시험이 적혀 있다는 사실만으로 현재 후보에서 실행을 통과했다고 판단하지 않는다.

## 정책 v2에 채택한 세 묶음

| 묶음 | 당시 권고안·채택 규칙 | 적용 영향 | 대안 | 채택 시점 |
|---|---|---|---|---|
| Q1 가격·계산 | 계좌 간 비교에 쓰는 가격은 허용된 공개 시세로 통일하고, 원본 시각 우선으로 선택. 원본 시각이 없는 입력은 유형을 구분해 수신 시각을 쓰는 기존 잔고 규칙 유지. 다른 계좌의 사적 KRW 시세는 공유하지 않음. 직접 가격이 충돌하면 해당 직접 경로를 제외하고 독립적인 유효 교차 경로는 허용. 계산 세부는 아래 A1–A13 권고안으로 명시 | 일부 값·부분합이 달라질 수 있음. 수신 순서나 계좌 조회 순서가 결과를 바꾸는 일을 줄임 | 현 C++ 동작을 그대로 명세화하고 Python만 맞춤. 단, 순서 의존 충돌과 잘못된 근거 허용은 결함으로 별도 처리 필요 | **채택 2026-10-21** |
| Q2 표시·유효시간 | 현재·과거 자산 평가 모두 180초. 참고 가격은 최대 24시간, 언제나 합계 제외. 소액은 `1 USDT`로 명시하고 숨긴 합계·제외 후 표시 합계를 툴팁에 표시. 거래소와 Solana의 SOL·USDC는 현재 identity를 유지하되 보관처를 이름에 구분 | 합계 자체는 표시 필터로 바뀌지 않음. 24시간을 넘은 참고액은 사라질 수 있음. 과거 60–180초 자료는 평가 가능해짐 | 참고 가격은 최대 나이 없이 기준 시각을 계속 표시. SOL·정식 USDC만 검증된 canonical mapping으로 한 줄에 합치고 하위 보관처를 분리 | **채택 2026-10-21** |
| Q3 가격 없는 자산 | 기본은 `미평가`. 사용자가 canonical Mint별 `평가 대상 제외`를 명시하고 해제할 수 있게 함. 목록 숨김은 값·완전 판정을 바꾸지 않음. 제외한 종수·범위는 총액 옆에 표시하고, 전체 자산의 순자산처럼 보이지 않게 제목도 평가 범위를 명시 | 가격 없는 소량 토큰 때문에 모든 화면이 막히는 문제를 사용자가 해소할 수 있음. 제외는 0원 평가와 다름 | 모든 보유 자산을 계속 평가 대상에 두고 가격 없는 한 `평가액` 유지. 자동 공개 목록 제외는 별도 기준 검증 전 도입하지 않음 | **채택 2026-10-21** |

24시간은 제품 정책이지 시장 표준으로 확인한 값이 아니다. 주말·휴장 중 공식 증권사 보고 평가금액을 임의의 오래된 공개 시세로 대체하지 않는다. 현재 증권 경로는 공급자가 보고한 KRW 평가액을 쓰므로, 공개 참고 시세의 최대 나이와 구분한다.

Q3에서 원래 보유 수량은 보존하고 사용자 제외 행을 일반 합계 제외와 구분한다. 제외 범위가 있으면 제목은 완전할 때 `평가 대상 합계`, 미완료면 `평가 대상 부분합`, 저장 화면은 `마지막 확인 평가 대상 합계`다. 제외 그룹 수를 함께 표시한다. 무조건적인 `순자산` 제목으로 전체 보유를 평가한 것처럼 표시하지 않는다.

## build 기록과 코드의 차이: 표 1–6

아래 표는 변경 전 build 50의 비교다. 해당 코드의 현재 대응 경로는 `fincept-qt/src/keefi/`이며, 표의 과거 C++/Python 차이를 현재 구현의 차이로 간주하지 않는다.

| ID | 변경 전 C++ / 문서·Python 관계 | 채택한 결정과 수정 범위 | 필수 확인 시험 |
|---|---|---|---|
| 표 1 / A11 | `AssetValuation.cpp:93–105`의 직접 가격은 출처 우선순위가 없음. `LocalWindow.cpp`는 선택 계좌 범위 또는 `public:` 가격을 남기고 그 전체를 value()에 전달한다. 따라서 같은 선택 범위 안에서 한 계좌의 KRW quote가 다른 계좌에도 쓰임. Python은 bundle의 모든 quote를 사용하며 이런 사전 scope 필터도 없음 | 공개 시세를 전체 계좌의 공통 비교 기준으로 사용. 계좌 quote를 쓰려면 자기 account scope에만 허용하는 별도 경로를 명시. 증권사 보고 평가금액 자체는 유지 | 계좌 A의 사적 BTC/KRW와 공개 BTC/KRW가 다르고 계좌 B도 BTC 보유: A quote가 B에 적용되지 않음. 연결/환경 필터 밖 quote 배제 |
| 표 2 | 직접 가격은 수신 시각, 교차·참고 가격은 원본 시각으로 선택. Python도 직접 가격은 수신 시각을 사용 | 유효한 후보 중 원본 시각 우선, 동일 원본 시각의 다른 가격은 충돌. 원본 시각 0을 허용하는 입력 유형과 fallback을 따로 정의. 최종 판정은 입력 순서와 무관 | 오래된 원본을 늦게 수신한 경우, 같은 원본의 충돌, 모든 입력 순열이 같은 결과 |
| 표 3 | `AssetsWindow.cpp:56` 과거 탭은 `AssetValuation::value()`의 기본 60000ms 사용. 현재 탭은 `DisplayFreshnessMs=180000`. Python은 180000ms | 과거 평가 시각 기준 180초로 통일. `flows()`의 역사적 입출금 환율 기본 60초까지 자동 변경하지 않음: G7 시간축 단계와 분리 | 과거 잔고 시각 대비 가격 60001/179999/180000/180001ms 경계. 현재·과거 같은 입력의 평가 일치 |
| 표 4 | 실제 소액 기준은 최신 업비트 KRW-USDT 가격인 1 USDT. UI 일부 안내·주석은 달러 표현이 남음. Python도 1 USDT | 1 USDT로 명시. USD 고정 환산으로 바꾸지 않음 | USDT가 1 USD와 다를 때 실제 KRW-USDT를 기준으로 숨김; 가격 없으면 필터 비활성 |
| 표 5 | `portfolioDust()`는 S(원 부분합), H(숨긴 합계), D=S−H를 계산하지만 UI는 H만 툴팁에 추가. Python은 D도 출력 | 상단 전체 부분합은 유지, 소액 필터 툴팁에서 S/H/D를 동일 범위로 표시. 계좌/종류 필터가 있으면 어느 범위인지 명시 | 필터 OFF/ON에서 상단 동일; H 정확한 합; D=S−H; 미평가·참고·음수·불확실 행은 소액 제외 대상 아님 |
| 표 6 | `PortfolioPresentation.cpp:23–27`은 거래소 `exchange:SOL/USDC`, 지갑 `solana:native`/정식 Mint를 별개 identity로 묶음. Python도 동일 | 기본 권고는 identity 유지+보관처 명칭 구분. 통합을 고르면 SOL·Circle 정식 USDC 같은 검증된 자산만 명시적 mapping으로 묶고 원래 Mint·보관처·수량/증거를 유지 | 심볼만 같은 다른 Mint는 합쳐지지 않음. 통합 전후 총 평가액 불변. 두 보관처의 하위 내역과 수량 보존 |

표 1·2·3·6은 실제 계산 또는 그룹 결과가 달라질 수 있다. 표 4·5도 정책 표시 선택을 기록하되 거래 원장·원시 금액은 바꾸지 않는다.

## 계산 세부 A1–A13

아래 비교는 **변경 전 build 50**의 C++와 Python 차이를 보존한 것이다. 권고 규칙은 Q1의 세부 명세로 채택했고 [평가 정책 v2](VALUATION-POLICY.md)에 반영했다. 아래 도입 당시 결과와 [현재 검증 상태](VERIFICATION-STATUS.md)를 구분한다.

| ID | 변경 전 C++ | 변경 전 Python / 실제 차이 | 채택한 규칙 | 겨냥할 시험 |
|---|---|---|---|---|
| A1 다리 자격 | `crossPrice()`/`solanaCrossPrice()`는 정확한 공개 scope, 가격 양수, complete, source·URL, 양의 원본/수신 시각, 두 시각 모두 as-of 이하를 요구. 최종 최소 시각에 180초 적용 | `leg()`는 venue/base/counter/양수/complete만 보고 Solana만 원본>0을 요구. 빈 URL·source, 미래 시각 등을 미리 걸러내지 않아, 다른 다리의 최소 시각에 가려질 수 있음. 참고 직접 가격도 C++보다 허용 범위가 넓음 | 각 다리의 출처·scope·URL·시각을 독립 검증하고 C++의 자격을 문서와 Python에 반영. 최신/참고의 나이는 선택 이후 별도로 판정. 미래 다리로 현재·참고값 생성 금지 | 한 다리만 미래, receipt 0, source 0, 빈 URL/source, wrong scope. 각 경우 유효한 다른 후보가 있으면 그것을 선택 |
| A2 Solana 계산 순서 | `truncate24(tokenUSD × wonPrice ÷ usdcUSD)`로 단위 가격을 만든 뒤 quantity를 곱함. `SolanaTokenData.cpp:114–125` | `truncate24(tokenUSD ÷ usdcUSD) × wonPrice`로 달러 비율을 먼저 버림. 정확한 끝자리가 다를 수 있음 | 현 C++ 순서를 보존하고 문서에 괄호와 버림 위치를 명시. Python은 그 규칙으로 독립 구현. 원 단위 반올림 전의 exact string까지 대조 | token=1, USDC=3, wonPrice=1000, quantity=1. C++ 규칙 단위 가격 `333.333333333333333333333333`; ratio-first 결과는 `333.333333333333333333333000` |
| A3 합성 근거 시각 | 모든 실제 다리의 원본 최소, 수신 최소. Binance USDC 다리이면 4개 실제 다리이며 파생 다리의 최소를 다시 취함 | build 49 대응 코드에서 같은 최소 규칙. README의 “세 다리” 설명은 Binance 우회 4개를 다 담지 못함 | 현 최소 규칙 유지. 실제 다리 목록을 모두 보존하고 문서를 3/4개 경로에 맞춤 | 가장 오래된 다리를 하나씩 바꾸며 결과 시각·legs 크기·180초 판정 확인 |
| A4 직접 충돌 | 순회 중 같은 receivedAtMs의 다른 문자열 가격을 만나면 즉시 반환. 더 최신 후보가 나중에 있어도 실패할 수 있어 입력 순서 의존. value()는 직접 실패 후 referenceValue()는 따로 시도 | 최신 receipt 후보만 모아 Decimal 값이 다른 경우 충돌. CONFLICT이면 교차·참고까지 중단 | 먼저 최신 원본 시각 후보군을 정하고 정규화 금액으로 충돌 판정. 충돌한 직접 경로만 버리고 독립 교차 경로는 허용. 현재 교차도 없으면 조건을 만족하는 참고 경로를 시도 | [old100, old101, newer102] 모든 순열; `1`과 `1.0`; 최신 동률 충돌+유효 교차; 충돌+참고만 존재 |
| A5 참고 충돌 | 참고 직접 후보의 최신 원본 시각에서 충돌하면 참고값 없음. conflict인 selected가 남아 교차 참고 경로도 사용하지 않음 | 직접 참고 충돌 시 교차 참고도 찾지 않아 이 부분은 일치. 다리/직접 후보 자격은 A1 차이 존재 | A4와 일관되게 충돌한 직접 경로를 버리고 독립 교차 참고 경로 허용. Q2의 참고 최대 나이 적용 | 직접 참고 동률 충돌+유효 교차 참고, 모든 참고가 최대 나이 초과, 참고와 현재 후보 혼재 |
| A6 정밀도 상한 | 입력과 최종 값은 정수/소수 각 80자리. decimal 연산은 정규화 문자열을 만든 뒤 범위를 검사. 전체 합이 초과하면 knownSubtotal이 빈 값으로 무효가 되며 순자산 없음. 개별 유효 row.value는 남음 | 개별 fits() 제한은 유사하지만 합계 초과 시 이전 subtotal을 유지하고 초과를 일으킨 행을 Unvalued로 둠. C++와 부분합·행 상태가 달라질 수 있음 | 입력·가격·행 결과의 상한과 합계 실패의 의미를 따로 명시. 양수·음수를 상쇄한 정확한 최종 합에 상한을 적용하고 입력 순서와 무관하게 판정. 표현 불가 합계는 unavailable, 개별 평가 행은 보존. 그룹 수량·평가액·참고 합계도 동일 규칙. Python은 None으로 직렬화 | 최대80자리+1 합계 초과, 소수160자리 곱, 끝0 정규화, 음수부채 상쇄, 입력 순열. 무효 총합을 0이나 이전 subtotal로 표시하지 않음 |
| A7 보고 통화 | reportedValue가 있으면 reportedValueCurrency를 자산처럼 직접/교차 환산. 빈 통화는 convert에서 실패 | 알려진 non-KRW 통화 환산 순서는 유사하지만 빈 reported currency를 `KRW`로 기본 처리 | 현 C++처럼 통화가 명시되어야 평가. 명시 USD 등의 실제 공개 환산 출처가 없으면 미평가. USDT·USDC를 USD와 같다고 놓지 않음 | USD 보고금액+직접 USD/KRW, 경로 없음, 통화 누락, KRW 명시, 원래 quantity가 달라도 reported 금액 우선 |
| A8 업체 손익 | `PortfolioPresentation.cpp:250–256`: 숫자·통화·계좌 scope·최신시각 외 complete/source/nonempty scope 요구 | provider_pnl()는 complete/source를 검사하지 않음 | 현 C++ 조건 유지. 손익은 표시용이고 자산 합계에 추가하지 않음을 명시 | complete=false, source 빈 값, 다른 account scope, 미래/만료 근거, 통화 누락, 정상 0 손익 |
| A9 보유 여부 | `assetPresence()`는 유효 비0가 하나라도 있으면 Held 우선. 없을 때 invalid가 있으면 NeedsCheck. 0행은 근거 불완전이면 NeedsCheck. Equity 0은 balanceComplete와 명시0 PnL을 추가 확인하고, Fresh가 아니면 Zero에서 NeedsCheck로 내림 | invalid가 하나라도 있으면 먼저 NeedsCheck. 일반0의 incomplete evidence와 Fresh 아닌 Equity0 강등도 동일하지 않음 | 현 C++의 비0 보유 우선, 불확실0은 NeedsCheck 규칙 유지. “수량 확실”과 “가치 미평가”를 별개로 표시 | quantity=1/reported invalid, quantity invalid/reported=1000, quantity0·evidence incomplete, equity0/누락 PnL/불완전/만료 |
| A10 정렬 | Zero 그룹 맨 뒤, 그 앞에 Fresh 그룹 평가액 내림차순, 나머지, identity 순. presence는 A9에 의존 | 같은 rank 구조. A9 차이가 있으면 최종 순서도 다름 | 현 정렬 유지. UI 헤더 정렬을 사용자가 선택한 경우와 기본 정렬을 구분 | 양수/음수/0/미평가/NeedsCheck 혼합, 동률 identity, A9 입력의 양쪽 순서 일치 |
| A11 다른 계좌 시세 | 선택한 snapshot scope 또는 public quote를 모아 value()에 전달. convert 자체는 개별 row account scope를 제한하지 않음 | bundle에 담긴 모든 quote를 씀. 연결/환경 밖 account quote도 들어갈 수 있음 | 표1 권고: 공개 가격 공통 사용, 사적 가격은 자기 계좌에 한정하거나 평가 입력에서 제외. 어느 선택이든 export bundle과 evaluator 규칙에 명시 | private A quote가 B에 미치는 영향, demo/삭제 연결 quote, 공개 동일자산 quote |
| A12 중복 범위·행 | 중복 accountScope의 뒤 snapshot은 통째로 건너뜀. 중복 component ID는 included=false·Unvalued 행으로 남김. 같은 valuationGroup+asset Equity 중복은 미평가로 차단 | accountScope 뒤 snapshot 건너뛰기는 일치. 중복 component ID 행은 출력에서 삭제. 같은 그룹/자산 Equity 중복 차단도 없음 | 중복은 전체 incomplete. 고유 첫 항목만 평가하고 나머지는 사유 있는 미평가 행으로 보존하는 C++ 방향. canonical source id와 diagnostics id 구분해 중복 행 식별 가능하게 함 | 같은id 다른값, 같은scope 두snapshot, 다른id 같은 Equity group/asset, 다른asset Equity 허용, 입력 순서별 선택 규칙 |
| A13 저장 화면 | `recordedPortfolio()`는 현재 연결과 맞는 snapshot만, 해당 scope/public quote만 선택. 시간 검증 후 기준시각=max. 저장 snapshot이 있는 연결만 complete로 다시 만들고 active=false. 참고 목록 안 씀. LocalWindow는 동일 실행의 검증된 동일 scope/완전 캐시만 표시 | recorded()는 관련 연결·quote를 먼저 거르지 않고 시간 검사하며 원래 connection state를 그대로 씀. LocalWindow의 verified scope gate도 모델링하지 않음 | 현 C++ 보안·scope·시각 조건을 정책에 명시. Python의 `--recorded`는 저장 평가 계산과 표시 허가를 구분하거나 허가 입력을 명시. R3에 따라 저장 화면의 live export는 계속 차단 | 저장값있는 not_read 연결, 저장값없는 complete 연결, 다른 연결의 미래 quote, scope 교체, stale quote가 저장 당시 이미 stale, 첫실행/재해제 구분 |

### A2의 버림은 표시 반올림과 별개

위 예시는 원 단위 UI에서는 두 결과가 모두 333원으로 보인다. 따라서 exact 금액 비교가 필요한 RC-04에서 차이가 난다. 이 차이를 허용 오차로 숨기지 않고 한 계산 순서를 확정한다. 최종 KRW 표시 반올림 규칙은 현재처럼 절댓값 기준 half-up을 유지하는 것이 권고안이다.

### 이미 수정된 R4와 혼동하지 않을 것

build 50의 R4는 계좌 `balanceComplete`와 개별 component evidence 판정을 분리하도록 문서·Python을 C++에 맞춘 변경이다. 정상 최신 행은 부분합에 남고 전체 순자산 판정은 미완료다. 이 규칙은 위 A9 보유 여부나 A12 중복 정책과 다른 축이다. R4를 다시 “계좌가 불완전하면 모든 행 미평가”로 되돌리지 않는다.

## CV-01 / CV-02: 가격 없는 자산과 원화 경로

| 상태 | 현재 동작 | 권고 동작 | 합계·표시 검증 |
|---|---|---|---|
| 메타데이터 실패, 가격 있음 | Mint identity로 보유·가격을 유지하고 이름 fallback 사용 | 유지. 메타데이터 성공을 평가의 선행조건으로 만들지 않음 | 정상 보유가 필터에서 사라지지 않고 가격으로 평가됨 |
| 가격 없음, 정상 보유 수량 | Unvalued, 완전 순자산 아님 | 기본 유지. 사용자 명시적 평가 제외를 선택했을 때만 대상 범위를 변경 | 제외 전 Unvalued, 제외 후에도 원본 보유·미평가 정보 보존; 평가 범위 제목·제외 종수 명시 |
| 실제 평가값 0 | Fresh 0과 미평가를 구분 | 유지 | 가격 없음→0 변환 금지; 수량0은 가격없이0 허용 |
| 목록의 미평가 숨기기 | 보여주는 행만 변경 | 유지 | 총액·부분합·complete 불변. 계좌 미조회 placeholder는 숨기지 않음 |
| 같은 심볼 다른 Mint | canonical identity로 분리 | 유지 | 심볼로 제외·가격 연결·합치기 금지. 네트워크와 Mint/정식 ID로 적용 |
| USD 등 원화 환산 경로 없음 | 보고 통화를 환산할 유효한 직접/허용 교차 quote 없으면 미평가 | 공식 USD/KRW 출처·시각·허용 경로가 정해질 때까지 유지. stablecoin=USD 가정으로 메우지 않음 | 통화 단위 확인, 경로 누락 이유 명시, 잘못된 동명/통화 quote 거절 |
| 업비트 체결 드문 가격 | 오래된 원본은 참고로 분리; Solana USDC는 build49 Binance 다리 선택 가능 | 원본 시각을 HTTP 수신 시각으로 새로 쓰지 않음. 이미 허용된 신선한 대체 다리를 선택 | 받은지1초/체결426초 quote는 Fresh 아님; 대체 다리 유효·실패 각각 판정 |

평가 제외 선택은 현재 열린 금고의 식별 범위에 연결한 공개 Mint 목록으로 저장한다. 선택 목록은 QSettings의 평문 설정이므로 암호화된 금고 자료로 설명하지 않는다. 잠금 시 메모리 목록을 비우며 잘못된 목록이나 저장 실패를 정상 선택으로 바꾸지 않는다. 설정에 잔고·금융 키·금고 암호를 넣지 않는다. 구현은 [ValuationPreferences.cpp](../../fincept-qt/src/keefi/ValuationPreferences.cpp), identity의 현재/legacy 경계는 [저장 형식](VAULT-FORMAT.md)을 따른다. 암호화·금고 형식을 이 기능 때문에 바꾸지 않으며 자동 스팸 목록 제외를 기본으로 적용하지 않는다.

## 결정 기록과 도입 당시 합성 결과

| 결정 묶음 | 채택 시점 | 적용 항목 | 코드·문서·Python 변경 | 도입 당시 C++ / Python 시험 | RC-04 대조 |
|---|---|---|---|---|---|
| Q1 | 정책 v2 채택 2026-10-21 | 표1·2, A1–A13 | AssetValuation.cpp/.h, recalc.py, 정책·README 수정 | Python 46/0·C++ assets 29/0 | 합성 C++ export/Python 비교 통과, 실계좌 미실행 |
| Q2 | 정책 v2 채택 2026-10-21 | 표3–6, 참고 최대 나이 | 코어24h·UI·과거180초 통합 | Python24h·C++ 코어 경계 통과, 일반 UI 180초/24h 만료 회귀 4/0 | 합성 대조 통과, 실계좌 미실행 |
| Q3 | 정책 v2 채택 2026-10-21 | CV-01·CV-02 | canonical 제외 인자·행 플래그·진단 입력·수량·제목 연동 | Python canonical 제외·동명 비제외, prefs10/0·scopeUI4/0 통과 | 합성 제외 bundle 비교 통과, 실계좌 미실행 |

Python 신규 G7 회귀를 먼저 추가해 실패 13건(subTest 포함)·오류 1건을 확인한 뒤 구현했다. 최초 통합에서 기존 시험을 포함한 44개가 통과했고, 후속 A6와 전체 미평가 헤더 시험을 포함해 46개가 통과했다. C++는 신규 `g7PricesUseOriginalTimePublicScopeAndOrderIndependentConflicts`, `g7ReferencePricesExpireAfterTwentyFourHoursAndInvalidLegsStayUnavailable`, `g7ExcludesOnlyExplicitCanonicalMintsWithoutErasingHoldings`, `g7ArithmeticKeepsExactDivisionExplicitCurrenciesAndDuplicateEquityRules`를 추가했다. 이 네 시험과 부호 상쇄 A6 시험을 포함한 assets 전체 29/0을 직접 실행했다. 이 수치는 정책 v2 도입 당시 합성 실행 결과이며 [현재 검증 상태](VERIFICATION-STATUS.md)와 구분한다.

## build 52 후속 — 공개 가격의 서버·PC 시계 차이

2026-10-21 build 52는 시각 판정 오류·미평가 증가 경로를 정정했다. 수신기가 이미 허용하던 원본 대비 수신 시계 차이 2,000ms를 공개 가격 평가에도 동일하게 적용한다. A1의 `원본 ≤ 평가 시각`, 양수 수신 시각·출처·scope·URL, 현재180초·참고24시간은 유지한다. 시각을 고쳐 쓰지 않는다. AP-01의 공개 FX만 같은 경계를 사용하고 계좌·모드 근거의 기존 조건을 넓히지 않는다.

직접·교차 가격의 0/3/19/35/1999/2000/2001ms 경계, 미래 원본·TTL, 공통 FX 실패의 다종목 전파, C++ export→Python 비교를 회귀로 고정한다. stream 후보 발효 예약과 일반 화면 캐시 재평가도 별도로 검증한다. 앞의 29/0·46/0은 build 51 당시 결과이며 [현재 검증 상태](VERIFICATION-STATUS.md)와 구분한다.

PL-01은 채택한 정책·날짜를 명시한 항목만 완료다. PL-02는 해당 C++와 Python 시험이 각각 있고 같은 입력의 결과가 일치해야 한다. PL-03은 이전 실계좌 묶음을 정책 결정 전후로 재평가할 권한·자료가 있을 때 별도로 수행하며, 이 소스 조사로 대신하지 않는다.
