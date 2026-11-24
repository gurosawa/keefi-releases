# KEEFI 연결 저장과 환경 정책

현재 연결은 이 PC의 암호화 금고에 저장한다. 이 문서는 [금고 형식](VAULT-FORMAT.md)이 참조하는 연결 보관 정책을 설명한다.

## 연결을 추가하는 조건

- 새 연결은 열린 금고에만 추가할 수 있다. 금고가 없거나 잠겨 있으면 먼저 금고를 만들거나 잠금을 해제해야 한다.
- `이번 실행만 사용` 연결은 생성하지 않는다. `Vault::addAccount`는 `EncryptedVault` 이외의 보관 방식을 거부한다.
- 금고 해제는 기존 연결을 읽는 동작이다. 새 연결은 별도의 저장 동작으로 추가한다. 저장 실패 시 금고를 잠근다.
- `storage_mode`의 과거 값이 모델에 남아 있는 것과 새 임시 연결을 허용하는 것은 다르다. 금고 payload는 암호화 보관 연결만 받아들인다.

## 환경과 조회 범위

새 연결은 서비스별 `live` 또는 `mainnet` 환경만 허용한다. 과거 `paper`·`demo`·`testnet` 연결은 읽기 호환과 삭제를 위해 보존하며, 현재 조회·평가 대상으로 삼거나 실전 연결로 자동 변환하지 않는다.

현재 KEEFI 빌드는 조회 전용이다. 연결이나 과거 주문 기록의 존재는 주문·정정·취소 기능을 활성화하지 않는다. 계좌 식별 방식과 저장 형식은 [금고 형식](VAULT-FORMAT.md)의 현재 계약을 따른다.

구현 근거: [Vault.cpp](../../fincept-qt/src/keefi/Vault.cpp), [AccountModel.cpp](../../fincept-qt/src/keefi/AccountModel.cpp), [조회 전용 빌드 정책](../../fincept-qt/src/keefi/BuildPolicy.h). 실제 실행 근거와 확인 범위는 [검증 상태](VERIFICATION-STATUS.md)에서 구분한다.
