# KEEFI Binance 선물 조건부 기록 호환

이 문서는 금고에 보존된 조건부 지정가 미확정 기록의 schema 9 계약을 설명한다. **현재 KEEFI 빌드는 조회 전용이며 조건부 주문을 신규 제출하거나 테스트·정정·취소하지 않는다.** 이전 문서의 주문 활성화나 직접 제출 절차는 현재 기능 안내가 아니다.

## schema 9의 구분

- 최상위 25개 필드를 유지하고 `futures`에 boolean `priceProtect`를 추가한다. 이 필드를 가진 기록은 schema 9가 필요하며, 새 11필드 terms를 schema 7·8로 위장한 자료는 거부한다.
- 기존 10필드 `futures` terms의 읽기 호환을 유지한다. 일반 주문의 `clientId/orderId`와 조건부 주문의 `clientAlgoId/algoId`는 별도 식별자다. 숫자가 같아도 하나의 주문으로 합치지 않는다.
- 과거 신규 조건부 기록(`kind=place`)은 `orderFamily=conditional`, `type=stop/take_profit`, `timeInForce=GTC`, 양의 수량·지정 가격·발동 가격, 명시적인 발동 기준과 진입/축소 의도, `priceProtect=false`를 요구한다.
- 이 과거 신규 형식에는 조건부 시장가·trailing·전량 청산·정정을 포함하지 않는다. 관측된 취소 기록이 허용하는 유형과 신규 기록의 허용 유형은 다르다.
- 형식 검증은 서버 접수나 최종 체결의 증명이 아니다. 응답이나 저장 ID가 있다는 이유로 미확정 결과를 성공으로 바꾸지 않는다.

## 저장 호환 범위

금고는 내부 schema 1~9를 읽으며 명시적 저장 시 표현 가능한 최소 schema 7·8·9를 선택한다. 열기만으로 기록을 새 스키마로 다시 쓰지 않는다. 새 필드를 포함한 금고를 그 형식을 지원하지 않는 이전 앱으로 덮어쓰지 않는다.

현재 컨테이너·암호화·이전 규칙과 모든 필드 제약은 [금고 형식](VAULT-FORMAT.md)을 따른다. 과거 `demo` 기록을 읽을 수 있어도 현재 `demo` 연결이나 조회를 허용하지 않는다.

구현 근거: [Vault.cpp의 선물 기록 검사](../../fincept-qt/src/keefi/Vault.cpp), [연결 환경](../../fincept-qt/src/keefi/AccountModel.cpp), [조회 전용 빌드 정책](../../fincept-qt/src/keefi/BuildPolicy.h). 과거 실행 결과를 현재 결과로 이어 쓰지 않으며 확인 범위는 [검증 상태](VERIFICATION-STATUS.md)에서 구분한다.
