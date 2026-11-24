# KEEFI 로컬 금고·원장 형식 — Dev101 전환 계약

2026-11-14 사용자 요청에 따라 제품의 번들 ID, 파일명, 암호화 컨테이너 식별자와 저장용 키 유도 domain을 KEEFI로 전환한다. 이 절은 Dev101의 현재 계약이다. 아래의 날짜가 있는 이전 형식 설명은 호환 검증을 위한 기록으로 보존한다. 실제 구현·시험·패키지 결과는 `receipts/KEEFI-DEV101`에 별도로 기록한다.

## 새 파일과 암호화 식별자

| 용도 | 현재 파일명 | 현재 8바이트 magic | 현재 저장용 key domain |
|---|---|---|---|
| 금고 | `credentials.keefi-vault` | `KEEFIVLT` | 기존 Argon2id13 암호 파생 키 |
| 자산 원장 | `assets.keefi`, `assets-current.keefi` 및 관련 보관본 | `KEEFIAST` | `KEEFI/assets-ledger/v1` |
| 스왑 기록 | `swaps.keefi` | `KEEFISWP` | `KEEFI/swap-journal/v1` |
| 가격 보관 | `prices.keefi` 및 종료 보관본 | `KEEFIPRC` | `KEEFI/price-archive/v1` |
| 가져온 기록 | `imported-records.keefi` | `KEEFIIMP` | `KEEFI/imported-records/v1` |
| 암호화 기록 내보내기 | `*.keefirtx` | `KEEFIRTX` | 패키지 암호의 Argon2id13 파생 키 |

금고와 내보내기 패키지의 외부 헤더는 72바이트, 원장 계열의 외부 헤더는 40바이트다. 기존 정수 인코딩·필드 길이·알고리즘·KDF 비용·크기 상한을 유지한다. 금고는 백업만으로도 논리 식별 방식을 보존하도록 현재 헤더 version을 2로 올리고 offset 14의 2바이트를 identity scheme으로 정의한다. 나머지 컨테이너의 version/reserved 규칙은 그대로다. 헤더는 AEAD additional data에 포함되므로 첫 8바이트만 치환해서 이전할 수 없다. 구 헤더로 인증하고 복호화한 다음 새 헤더와 새 nonce로 다시 암호화한다. 저장용 domain이 다른 원장은 같은 원장 root key에서 새 domain으로 키를 유도한다. 인증 실패 뒤 다른 domain을 추측하여 재시도하지 않는다.

| 금고 헤더 | offset 8의 version | offset 14의 값 | 인증 후 해석 |
|---|---:|---:|---|
| 현재 KEEFIVLT | 2 | 1 | keefi-v1 |
| 현재 KEEFIVLT | 2 | 2 | legacy-v1 |
| 이전 형식 | 1 | 0 | legacy-v1 |

허용하지 않은 version·scheme 값과 미출시 KEEFIVLT version 1은 거절한다. 헤더의 식별 정보는 암호와 payload 인증이 성공한 뒤에만 사용한다. 정상 프로필의 Vault는 시작 때 선택된 scheme과 다른 금고의 unlock/즉시 복원을 거절한다. 암호 변경과 계좌 저장은 인증된 scheme을 유지한다.

금고 이전은 기존 암호·salt·opslimit·memlimit·암호화된 계좌/인증정보/pending 원문과 랜덤 원장 root key를 보존한다. 이때 새 암호 정책을 소급 적용하거나 원장 키를 새로 만들지 않는다. 새 nonce와 새 magic을 사용한 인증 암호문을 생성하고 다시 열어 원문 및 키 보존을 확인한다. 새로운 금고 생성이나 사용자가 승인한 암호 변경의 기존 정책은 그대로 적용한다.

## 프로필 이전과 쓰기 경계

Dev의 새 기본 활성 위치는 플랫폼 data root 아래 `KEEFI/Profiles/Dev`다. Dev100의 `KEEFI/KEEFI Dev`와 더 오래된 제품의 프로필은 이전 입력이다. 이름만 KEEFI인 Dev100 폴더도 내부 저장 형식이 구형이므로 새 활성 프로필로 직접 사용하지 않는다.

이전은 일반 금고·계좌 backend를 시작하기 전에 수행한다. 원본의 실행·파일 잠금과 소유권을 확인하고, 원본 밖의 전용 stage에 복사하고, 원본과 복사본의 목록·크기·SHA-256·권한을 검증한다. 사용자가 입력한 기존 암호로 stage만 변환한다. 변환 결과를 다시 열어 검증한 뒤 소유권이 확인된 새 목적지에만 게시한다. 기존 목적지를 덮어쓰지 않는다. 거절·취소·잘못된 암호·불완전한 파일·다른 실행의 잠금·원본 변경이 있으면 구 프로필에 새 writer를 연결하지 않는다.

`--profile-dir`는 명시한 경로만 검사한다. 기존 형식을 발견하면 사용자의 선택을 거쳐 별도 sibling 경로로 옮긴다. 기본 개인 프로필을 몰래 검색하지 않는다. 활성 marker는 `.keefi-profile.json`, format은 `keefi-profile/2`이며 검증 결과·identity scheme·출처 종류·원본 manifest SHA를 담는다. 암호·credential·원본 절대 경로는 marker에 넣지 않는다.

일반 `Vault::unlock`과 `open(false)`는 읽기 동작이다. 구 자료를 읽었다는 이유로 자동 변경하거나 잠금 해제 중 파일을 다시 쓰지 않는다. 구 자료는 전용 이전 경계에서만 현재 형식으로 전환한다. 현재 형식 백업을 동일 scheme으로 복원할 때는 암호문 바이트를 그대로 보존한다. 구형 백업은 원본을 보존하면서 새 magic/version/scheme/nonce로 인증 재암호화한다.

서로 다른 scheme의 백업은 현재 backend에서 즉시 활성화하지 않는다. 금고와 암호화 저장소가 없는 프로필에서만 비활성 복원본을 준비하고 정상 종료를 안내한다. 다음 실행은 backend 생성 전에 준비 기록·소유권·파일 정체성·암호문 digest와 대상 상태를 검증한 뒤 복원을 게시한다. 이전의 빈 프로필과 그 공개 설정도 보존한다. 실행 중인 프로필의 전역 identity scheme을 바꾸지 않는다. 실패한 복원 준비가 즉시 복원으로 우회하거나 기존 금고를 덮어쓰는 경로는 없다.

## 기록의 식별자와 증거 보존

저장용 암호화 domain과 논리적인 계좌·이벤트·중복 판정용 identity domain은 서로 다른 계약이다. 새 프로필은 `keefi-v1` identity를 쓴다. 이미 기록이 있는 구 프로필을 가져오면 `legacy-v1` identity를 유지하여 기존 계좌 scope, 이벤트 참조, 설정 결합, 내보낸 기록의 중복 판정이 달라지지 않게 한다. 기존 해시에서 원래 계좌번호나 거래소 식별자를 역산하여 새로운 해시로 바꾸지 않는다. 이 호환 상수는 명시적인 legacy reader/identity 경계에만 남긴다. 실제 저장 컨테이너와 새 파일명은 KEEFI다.

자산 원장의 schema 3 보관 증거는 원래 암호문의 SHA를 역사 증거로 보존한다. 파일명을 새 확장자로 바꾸고 재암호화했을 때는 `migration.rewriteEvidence`의 version 1에서 원본 `sourceDigest`와 현재 `cipherDigest`를 별도 기록한다. 새 암호문의 SHA를 원본 증거인 것처럼 덮어쓰지 않는다. 보관본의 원문·순서·계산 결과는 유지한다.

사용자가 작성한 별칭·메모·라벨에 들어 있는 과거 제품명은 사용자 데이터다. 이름 전환을 이유로 임의 치환하지 않는다. 과거 게이트·설치 영수증·합성 legacy fixture와 그 SHA도 원본 증거로 보존한다. 이 계약은 개인 금고를 에이전트가 여는 허가가 아니며, 자동 검증은 합성 자료만 사용한다.

---

## 이전 형식의 기록 — 로컬 금고 v1 · 내부 스키마 v7/v8/v9

2026-10-21 소스 변경: 모든 업체의 모의·테스트넷(`paper`·`demo`·`testnet`) 지원을 종료했다. 신규 등록·별칭 변경·인증정보 교체는 실전(`live`)과 Solana `mainnet`만 허용한다. 예전 은퇴 환경 연결과 미확정 주문 기록은 금고의 읽기 호환을 위해 보존한다. 다른 실전 연결을 추가·삭제할 때도 은퇴 연결을 재저장할 수 있으며 `계좌·연결 → 연결 삭제`로 지울 수 있다. 은퇴 연결을 실전으로 자동 변환하거나 조회·평가하지 않는다. 아래 환경별 스키마 허용값은 예전 기록을 해석하는 계약이며 새 연결 허용 범위가 아니다. 파일 형식·암호화·KDF는 바꾸지 않았다.

2026-10-18 Dev 소스 변경: [G6 방향 결정](G6-DIRECTION.md)에 따라 `이번 실행만 사용`(session-only) 연결 생성을 폐지했다. `addAccount`는 열린 금고의 `EncryptedVault`만 허용하고 `saveAccountToVault`를 제거했다. 파일 형식·암호화·비용 매개변수와 `storage_mode` 필드 해석은 바꾸지 않았다. 아래 session-only 설명은 설치된 build 23 이하와 이전 소스의 동작 기록이다.

2026-10-17 후속: [로컬 사용성 보완](G3-LOCAL-USABILITY.md)은 임시 연결을 사용자의 명시적인 **금고에 저장** 동작으로 암호화 보관한다. 같은 credentialRef의 공유 연결을 함께 전환하며 계좌 ID·참조·미확정 기록·원장 키와 기존 파일 형식을 유지한다. 금고 생성·잠금 해제만으로 임시 연결을 저장하는 정책 변경은 없다.

G1은 libsodium 1.0.22의 Argon2id13과 XChaCha20-Poly1305를 사용한다. 금고는
PC별 파일이며 Azure·Git 동기화 대상이 아니다. 이전 Fincept 저장소를 자동 이전하지 않는다.

## 파일

정수는 little-endian이다. 아래 72바이트 헤더 전체를 AEAD additional data로 인증한다.
뒤에는 암호문과 16바이트 인증 태그가 이어진다.

| Offset | 길이 | 의미 |
|---|---:|---|
| 0 | 8 | `FINIXVLT` |
| 8 | 2 | format version = 1 |
| 10 | 2 | KDF identifier = 1 (Argon2id13) |
| 12 | 2 | cipher identifier = 1 (XChaCha20-Poly1305) |
| 14 | 2 | reserved = 0 |
| 16 | 4 | 실제 opslimit |
| 20 | 8 | 실제 memlimit, 바이트 |
| 28 | 16 | 금고 생성·암호 변경 때 새 랜덤 salt |
| 44 | 24 | 매 저장마다 새 랜덤 nonce |
| 68 | 4 | 암호문과 태그의 총 길이 |

새 금고의 비용은 opslimit 3, memlimit 67,108,864바이트(64 MiB)다.
읽기 전에 opslimit 3–6, memlimit 64–256 MiB, 전체 파일 최대 8 MiB를 확인한다.
지원하지 않는 버전·알고리즘·reserved 값·길이·비용을 추정하거나 낮춰서 열지 않는다.

### 현재 암호 입력 정책 (Dev 0.16.5/build 56)

Vault core는 공백 제거·Unicode 정규화 없이 암호 원문을 UTF-8로 처리한다. 새 암호는
printable ASCII(`U+0020`–`U+007E`) 12자 이상, UTF-8 최대 1024바이트이며 영문 대문자 `A-Z`,
영문 소문자 `a-z`, 숫자 `0-9`, ASCII 특수문자를 각각 하나 이상 포함해야 한다. 특수문자는
`!`부터 `/`, `:`부터 `@`, `[`부터 백틱, `{`부터 `~`까지의 ASCII 구두점이며 공백은 세지 않는다.
기존 금고의 잠금 해제와 암호 변경 시 현재 암호 인증에는 새 복잡도·ASCII 조건을 적용하지 않아
이전 UTF-8 암호의 core 인증 호환성을 유지한다. 이 계약은 아래 UI 입력 정책과 구분한다.
이번 UI 변경은 KDF·AEAD·파일 형식을 바꾸지 않으며 마스터 암호와 파생 키는 파일에 저장하지 않는다.

Dev 0.16.5/build 56 소스의 UI 입력 정책은 금고 생성·잠금 해제·현재 암호·새 암호·새 암호 확인
모두에 printable ASCII만 허용한다. 한글은 두벌식 영문 키로 자동 치환한다(예: `안녕` → `dkssud`,
`값` → `rkqt`). 기존 ASCII 문자의 대소문자와 앞뒤 공백은 그대로 보존하며, 그 밖의 문자는
해당 편집 전체를 거부하고 이전 입력을 유지한다. 사용자가 기존 암호는 ASCII라고 확인했으므로
기존 한글 암호 체크박스는 제거한다. 이는 core의 기존 UTF-8 암호 인증이나 저장된 암호를 바꾸는
변경이 아니다. 누르는 동안만 암호를 표시하며, 한글 변환 설명·정상 변환 알림은 화면에 두지 않는다.
실제 입력 오류만 알리고 창 높이를 내용에 맞춘다. [build 56 동작·검증 범위](G7-BUILD56-LOADING-LAYOUT.md)를 따른다.

## 암호화되는 내용

평문 프레임은 `FVP1`, 32비트 metadata 길이, canonical Qt compact JSON,
32비트 credential frame 수, 각 credential 프레임으로 구성한다. metadata도 암호문 안에 있다.
G3 자산 원장 단위부터 JSON `schema_version: 7`, `accounts`, `credential_sets`, `pending_operations`를 쓴다.
G1의 schema 1(`accounts`), G2의 schema 2, 토스·한국투자의 schema 3, 키움·LS의 schema 4, OKX의 schema 5, 공유키의 schema 6도 읽는다.
이전 금고를 여는 것만으로 파일을 다시 쓰지 않으며, 다음 명시적 저장부터 내부 schema 7을 쓴다.
**schema 7은 이전 앱에서 거부된다. 이전 앱으로 되돌려 덮어쓰지 않는다.**
외부 파일 헤더·KDF·AEAD 형식은 그대로다. credential 프레임은
필드 수, 필드마다 이름 길이·이름·값 길이·값을 담는다. credential 값은 JSON에 넣지 않는다.

계좌 최대 64개, 계좌별 credential 최대 16개, 값당 최대 8192바이트,
metadata 최대 128 KiB다. 알 수 없는 필드, 중복 ID, 예상하지 않은 상태, 남은 바이트는
거부한다. session-only 계좌는 저장 프레임에서 제외한다.

G2의 pending_operations는 최대 128개의 미확정 주문 기록이다. 금고 안에만 암호화해 저장하며
주문 전송 전에 원자 저장을 마친다. 고유 요청 ID, 로컬 연결 ID, API key의 SHA-256 식별 지문,
서비스·환경·종목, 요청 종류, 원주문/새 주문 client ID, 상태, 작성 시각, 가격·수량·총액·방향·유형만 허용한다.
금융 Secret이나 인증 헤더는 기록하지 않는다. 이 지문은 인증 수단이 아니며 로그·검증 내보내기에도 넣지 않는다.
G2 현물 기록은 다른 키의 응답으로 해소하지 않는다. 같은 키를 재입력한 연결에서 대사할 수 있다.
같은 서비스·환경에 미확정 기록이 있으면 신규·정정을 막고, 원래 키의 조회 대사가 성공해야 해제한다.
키를 폐기한 상태에서 남은 기록의 재귀속은 자동으로 수행하지 않는다.

schema 3은 위 G2의 17개 필드 기록과 토스·한국투자 국내주식의 20개 필드 기록을 같은 배열에 보존한다.
기존 17개 필드 기록은 `upbit`·`binance` 현물 기록이며 `market`을 추가하거나 원문을 고치지 않는다.
schema 2에서는 이 기존 기록만 허용한다. G3의 새 기록은 기존 17개 필드에 다음 세 필드만 추가한다.

| 필드 | 검증·의미 |
|---|---|
| `market` | `domestic_equity` 고정 |
| `accountScope` | 소문자 16진수 정확히 64자. 인증된 계좌 범위를 묶는 해시이며 인증정보 자체가 아님 |
| `operationOrderId` | 아직 응답을 받지 못하면 빈 문자열. 수신하면 영문·숫자·`_`·`-` 최대 128자. 정정·취소가 새로 발급한 작업 ID이며 원주문 `orderId`와 별도 |

새 20개 필드 형식은 `provider: toss`의 `environment: live`와 `provider: kis`의 `live`·`paper`를 허용한다. 다른 서비스·시장·환경의
확장은 별도 구현 전까지 거부한다. 필드 누락·추가, 잘못된 계좌 범위 해시, 중복 기록 ID는 거부한다.
`accountSeq`·`accountNo`·OAuth 토큰·인증 헤더는 이 기록에 저장하지 않는다. 계좌 범위 해시도 암호문
안에만 저장하며 로그·비민감 검증 기록에 내보내지 않는다. 금고의 형식 검사는 해시가 실제 선택 계좌에서
만들어졌음을 입증하지 않으므로, 컨트롤러가 선택 계좌와의 일치 및 서버 주문 내용을 별도로 검증한다.

G3 토스 계좌 scope는 공식 계좌 목록의 `accountNo`·`accountType`·`accountSeq`를 서비스·환경과 함께
길이 구분하여 해시한 값이다. 최초 요청 credential의 지문은 감사 정보로 보존하며 새 Client ID로
덮어쓰지 않는다. 키 교체 후 대사는 이번 세션의 전체 계좌 조회가 성공했고 선택 scope가 원래
기록과 같을 때 허용한다. 그 뒤에도 서버 주문 ID와 원 요청 내용·원주문/작업 상태를 검사해야
미확정 기록을 해소한다. sequence 또는 계좌 identity가 달라지면 자동으로 같은 계좌로 합치거나
기록을 재귀속하지 않는다. 계좌 목록의 번호 원문은 금고 주문 기록·UI·비민감 출력에 남기지 않는다.

한국투자의 scope는 서비스·환경·CANO·계좌상품코드를 길이 구분하여 해시한다. 사용자가 입력한
계좌 설정이나 토큰 발급만으로 검증된 계좌로 취급하지 않으며, 이번 세션에서 해당 계좌의
보유 조회 전 페이지를 성공적으로 받은 뒤 대사를 허용한다. 주문 참조는
`K1_YYYYMMDD_BRANCH_ODNO_VENUE`로 날짜·지점·주문번호·거래소를 함께 보존하고 숫자 ID의
앞자리 0을 제거하지 않는다. 저장된 정정·취소의 `operationOrderId`와 원주문 `orderId`는
같은 검증 계좌에 속할 때만 관계 문맥으로 복원한다. 이후 공식 조회의 원주문 번호·날짜·거래소와
요청 조건까지 일치해야 해소하며, 사용자가 임의 연결한 ID는 저장된 접수 증거로 승격하지 않는다.

schema 4는 기존 17·20필드의 원문을 보존하면서 키움 `live`·`paper` 국내주식의
23필드 기록을 추가한다. broker 20필드에 아래 세 필드를 정확히 더한 형식이며,
schema 2·3에는 키움 기록을 넣을 수 없다. 토스·한국투자 기록에 새 필드를 끼워 넣는 것도 거부한다.

| 추가 필드 | 의미 |
|---|---|
| `quantityMode` | `exact` 또는 `all_remaining`. 신규는 exact만 허용. 잔량 전체 취소·정정에서도 quantity는 확인한 양의 정수 잔량이며, wire의 0과 구분한다. |
| `baseOriginalOrderId` | 공식 mutation 응답의 모주문 참조. target orderId 및 이번 operationOrderId와 별도. 미수신이면 빈값. |
| `baseOriginalProvenance` | 빈값 또는 `broker_receipt`. broker_receipt는 모주문·작업 접수 ID가 모두 있을 때만 허용. 사용자 수동 연결은 이 값으로 승격하지 않는다. |

키움 참조는 `W1_YYYYMMDD_VENUE_RAWNUMBER`이며 유효 날짜, KRX/NXT/SOR, 최대 20자리
양의 ASCII 숫자 원문을 검사하고 선행 0을 보존한다. 지원 거래소와 실제 주문 가능 여부는
어댑터가 더 좁게 검사한다. 최초 write-ahead에는 mode와 확인 수량을 쓰고 접수·모주문을
비워 둔다. mutation 응답 뒤 접수·모주문·provenance를 한 번의 원자적 저장으로 갱신한다.
형식 오류·미확인 mode/provenance·누락/추가 필드·저장 실패는 기존 암호문을 보존한다.
신구 17/20/23필드 기록을 같은 배열에 보존하며 레거시 기록을 새 의미로 추정 변환하지 않는다.

LS 국내주식 `live`·`paper`도 schema 4의 정확히 23필드 형식을 사용한다. 수량 모드는
`exact`만 허용하고 quantity는 최대 16자리의 양의 정수 문자열이다. 취소에서 quantity는
검토한 **잔량**이며 wire에도 같은 양수를 보낸다. 키움의 `all_remaining` 및 wire 0 의미를
LS에 적용하지 않는다. LS 참조는 `L1_YYYYMMDD_VENUE_RAWNUMBER`로 유효 날짜,
KRX/NXT/SOR, 최대 10자리 양의 ASCII 숫자를 검사하며 선행 0은 보존한다.
모주문과 접수 ID·`broker_receipt`의 동시성 및 사용자 수동 연결의 제한은 키움과 같다.
LS 필드를 17/20필드 또는 키움 기록에 끼워 넣지 않는다. LS 기록이 저장된 schema 4는
키움까지만 지원하는 이전 앱에서 거부되므로 구버전으로 금고를 덮어쓰지 않는다.

LS scope는 토큰에 연결된 공식 계좌 조회의 전체 계좌번호를 provider·선택 환경과 함께
길이 구분하여 해시한다. 계좌 확인값은 선택자가 아니며 발견 계좌와 정확히 비교한다.
상세 계약은 [LS 구현 명세](G3-LS.md)를 따른다. paper scope 구분은 FINIX의 기록 격리이며
같은 REST origin에서 서버의 실제 실행 환경을 입증하지 않는다. 확인 전 모의 mutation은 차단한다.

schema 5의 OKX 계좌 metadata는 기존 10필드에 `region` 하나만 추가한다. 값은
`global`·`us`·`eea`이며 신규 등록에서 반드시 선택한다. schema 1~4의 10필드 OKX 계좌는
지역을 추정하지 않고 빈값으로 읽으며 연결을 차단한다. 사용자 선택과 키 3개 재입력으로
지역·인증정보를 같은 원자 저장에서 교체한다. 기존 3인자 키 교체 API는 저장된 지역을 보존한다.
다른 provider의 region, 빈 region을 가진 11필드 metadata, 알 수 없는 필드는 거부한다.

OKX 미확정 기록은 기존 17필드에 다음 10필드를 더한 정확히 27필드다.
`market`, `region`, `accountScope`, `amendRequestId`, `quantityUnit`, `ackState`,
`beforePrice`, `beforeQuantity`, `beforeExecuted`, `beforeUpdatedAt`을 문자열로 저장한다.
provider는 `okx`, market은 `spot`, environment는 `live`·`demo`만 허용한다.
scope는 `finix.okx.account.v1`·지역·환경·spot·정확한 UID 원문을 각 UTF-8 길이와 함께
SHA-256으로 묶은 64자리 해시다. UID 원문·토큰·키·인증 헤더는 pending에 넣지 않는다.

정정은 동일 orderId를 유지하고 별도 32자리 amendRequestId 및 변경 전 가격·수량·체결량·시각을
보존한다. `ackState`는 `not_received`·`accepted`·`rejected`·`unknown`이며 ACK 접수 자체는
변경 완료 증거가 아니다. 서버 대사가 별도로 필요하다. 신규·취소는 before 필드를 비운다.
`quantityUnit=quote`인 시장가 주문은 quoteAmount에 원문 sz를 쓰고 quantity는 비워 둔다.
base 체결량을 quote 주문액에서 빼지 않는다. 신규 시장가 매수는 quote, 매도는 base이며,
외부 주문 취소는 관측된 단위를 그대로 보존한다. 외부 post_only·ioc·fok 취소 형식도 보존한다.
17·20·23필드의 기존 기록과 혼합 보존하며 schema 1~4에서 OKX 27필드를 읽지 않는다.

session-only 키로도 금고가 열려 있으면 주문할 수 있다. 키 자체는 여전히 저장하지 않고
위 주문 기록만 저장한다. 금고 없이 session-only 연결은 조회·공식 주문 테스트까지만 허용한다.
대사로 결과를 확인한 기록은 pending 배열에서 제거한다. 별도의 장기 거래 원장을 구현한 것은 아니다.

## schema 6 공유 인증정보와 USDⓈ-M 기록

계좌 metadata에는 `credentialRef` UUID가 필수다. 실제 키·키 해시·거래소 UID가 아니며
같은 문자열이 계좌 동일성을 입증하지 않는다. `credential_sets`의 각 항목은 정확히
`id`, `provider`, `environment`, `region`, `storage_mode` 다섯 문자열이다. schema 6의
FVP1 metadata 뒤 count와 binary credential frame은 계좌 수가 아니라 참조별 인증정보 수다.
계좌 순서에서 처음 등장한 참조 순서로 한 번씩 기록하며 비밀 값은 여전히 SecureBuffer와
binary frame에만 둔다. credential set과 계좌는 각각 최대 64개이고 기존 파일·metadata 상한을 유지한다.

내부 Entry는 불변 인증정보 객체를 공유한다. 기존 addAccount는 새 UUID·키·연결을 한 번에 저장하며
외부에서 지정한 credentialRef를 받아 기존 키를 덮어쓰지 않는다. `addAccountUsingCredentials`는
같은 보관 방식의 Binance 실전 현물↔USDⓈ-M만 허용한다. 같은 참조·시장의 중복 연결,
다른 provider·환경·지역·보관 방식의 공유를 거부한다. testnet 현물과 demo 선물은 독립 키다.
최초 한 번에 두 시장을 등록하는 흐름은 제공하지 않는다.

공유 키 교체는 같은 참조를 사용하는 모든 연결을 새 불변 객체로 갱신한 뒤 한 번 원자 저장한다.
성공 또는 실패 후 잠금의 기존 계좌 변경 신호가 거래 세션·준비 상태를 폐기한다. 한 연결 삭제는
다른 참조 연결의 키를 남기며 마지막 참조 삭제 때 버퍼도 해제한다. session-only 공유 연결과
키는 파일에서 모두 제외하고 잠금 때 전부 소거한다. pending 기록은 연결/키 삭제와 함께 지우지 않는다.

schema 1~5는 기존 1:1 frame을 검증한 뒤 메모리에서만 각 accountId를 credentialRef로 붙인다.
unlock 자체는 파일을 쓰지 않고 같은 키 문자열의 기존 연결도 자동 병합하지 않는다.
계좌 ID·별칭·시장·기존 pending 원문을 보존한다. 오래된 futures/testnet 설정은 읽어 보존하되
새 등록에서는 spot=live/testnet, futures=live/demo만 허용한다. 자동 환경 변환은 없다.
schema 6에서 없는 ref·중복 set ID·사용되지 않는 set·계좌와 set의 provider/env/region/storage
불일치를 거부한다. 계좌가 참조하는 set은 존재해야 하지만 pending의 과거 ref는 삭제돼도 유효하다.

선물 pending은 정확히 25필드: 기존 17필드에 `market`, `credentialRef`, `orderFamily`,
`futures`, `before`, `algoId`, `clientAlgoId`, `actualOrderId`를 추가한다. provider는 binance,
market은 usdm_futures, environment는 live/demo다. 기존 market 없는 Binance 17필드는 현물이며
선물과 섞어 대사하지 않는다. 기존 broker 20/23·OKX 27필드도 그대로 보존한다.

`futures`는 positionSide/exposure/reduceOnly/closePosition/timeInForce/workingType/triggerPrice/
activationPrice/callbackRate/modifyId의 정확히 10키다. reduceOnly·closePosition만 bool이고 나머지는
문자열이다. `before`는 price/quantity/executed/updatedAt 네 문자열이며 정정에만 유효값을 쓴다.
일반 주문은 orderId/clientId, 조건부 주문은 algoId/clientAlgoId/actualOrderId를 별도로 저장한다.
동일 숫자 ID도 같은 주문으로 합치지 않는다. 신규는 일반 LIMIT/MARKET만, 정정은 일반 LIMIT만,
조건부 기록은 확인된 STOP/TP/TRAILING 유형 취소만 허용한다. 일반 시장가 price와 불필요한
조건부 필드는 비우고, 정정 modifyId는 양의 int64 십진수이며 새 전체 수량은 기존 체결량보다 커야 한다.
접수 ACK만으로 정정 완료를 기록하지 않는다. 상세 동작은 선물 컨트롤러의 조회 대사를 따른다.

## 저장과 잠금

schema 7은 schema 6 credential frames 뒤에 little-endian uint32 원장 키 길이와 키 bytes를
붙인다. 길이는 정확히 0 또는 32다. 키는 JSON·계좌 credential·pending에 넣지 않는다.
schema 1~6을 열 때는 키가 없는 상태로 유지한다. `withAssetsLedgerKey(false, callback)`은
키를 만들거나 저장하지 않는다. 명시적으로 원장 생성을 요청한 경우만 새 랜덤 32바이트를
원자 저장한 뒤 빌려준다. 콜백은 API credential과 동일하게 재진입·비동기 보관을 금지한다.
키는 금고 암호 변경 때 그대로 재암호화되므로 별도 자산 원장을 다시 쓰지 않아도 된다.
자산 원장에는 이 키를 직접 쓰지 않고 keyed BLAKE2b-256(`crypto_generichash`)에
`FINIX/assets-ledger/v1`을 넣어 용도별 키를 유도한다. 별도 파일 형식과 계산 경계는
[자산 원장](G3-ASSETS.md)을 따른다. 잠금은 랜덤 원장 키와 원장의 파생 키를 모두 지운다.

단일 프로필 인스턴스와 열린 금고의 파일 잠금을 적용한다. 외부 변경은 이전 암호문의
SHA-256과 비교해 덮어쓰기를 중단한다. 동일 폴더의 사용자 전용 임시 파일에 암호문만
쓰고 flush/fsync, 복호화·원문 비교를 거친 뒤 원자 교체한다. Mac에서는 폴더 fsync와
최종 파일 읽기도 확인한다.

교체 전 실패는 기존 파일을 보존한다. **교체 후 디스크 반영·읽기 확인 실패는 새 파일이
이미 존재할 수 있으므로 결과를 확정하지 않고 잠근 뒤 다시 열도록 안내한다.** 이 구분은
모든 저장 실패에 이전 파일이 남는다고 주장하지 않기 위한 것이다. 전원 차단 상황 전체의
내구성을 테스트했다고 표현하지 않는다. 이전에 복사한 금고는 당시 암호로 열릴 수 있다.

보호 버퍼는 sodium_malloc, 성공 여부를 검사한 sodium_mlock, sodium_memzero를 사용한다.
금고 잠금은 session-only 입력도 제거하고 진행 중 worker를 중지시킨다. UI 문자열,
Qt/Python 내부 복사본, 클립보드, 관리자 덤프까지 완전 소거를 보장하지 않는다.
OS 전체 설정을 바꾸지 않으며 프로세스별 덤프 제한과 감지 가능한 설정 검사를 적용한다.

키움 계좌 범위 해시는 인증 후 반환된 전체 계좌번호와 provider·환경을 결합한다. 식별자 정규화·계좌 선택 검증의 상세 계약은 [키움 구현 명세](G3-KIWOOM.md)를 따른다. 키·계좌번호 원문은 pending 기록에 넣지 않는다.

## G3 보완의 schema 8 주문 기록

새 Binance 현물 취소·신규 정정에는 `replaceMode=cancel_replace`, `replacementState`를, 묶음 취소에는 `orderListId`를 암호화한 주문 원장에 추가한다. 토스 별도 조건 감시 취소에는 `brokerConditional=true`(문자열)를 추가한다. 서로 다른 ID 의미를 기존 원주문 정정이나 일반 취소로 해석하지 않는다.

읽기는 schema 1~9를 허용한다. 일반 계좌·기존 주문 저장은 schema 7을 유지하고, 위 새 형식의 미확정 주문이 있을 때 schema 8을 쓴다. schema 8의 자료를 schema 7로 위장한 파일은 거부한다. 새 형식의 미확정 주문을 해소한 뒤 기존 표현만 남으면 schema 7로 저장할 수 있다. 열기만으로 다시 쓰지 않으며 파일 헤더·KDF·AEAD·랜덤 원장 키는 바꾸지 않는다. schema 8 미확정 기록이 남은 금고는 이전 앱에서 열리지 않으므로 이전 앱으로 덮어쓰지 않는다.


## schema 9 조건부 지정가 신규

사용자가 승인한 [Binance 선물 조건부 지정가](G3-CONDITIONAL-LIMIT.md)는 25개 필드를 유지하면서 `futures.priceProtect`의 boolean을 추가한다. 이 필드를 가진 관측 조건부 취소도 schema 9를 쓴다. 기존 10필드 futures terms는 계속 읽으며 새 11필드 terms를 이전 스키마로 위장할 수 없다.

신규 조건부 기록은 `kind=place`, `orderFamily=conditional`, `type=stop/take_profit`, `timeInForce=GTC`, 양의 수량·지정 가격·발동 가격, 명시적 발동 기준·진입/축소 의도와 `priceProtect=false`를 요구한다. 일반 client/order ID는 비어야 하며 clientAlgoId는 필수, algoId는 응답 전 빈값이고 응답 후 양의 int64다. 시장가·trailing·closePosition·정정은 이 신규 형식에 허용하지 않는다. 기존 감소 조건과 before/modifyId 경계도 유지한다. schema 7로 표현 가능하면 7, 아니면 8, 이 확장이 필요할 때만 9를 저장한다. 새 미확정 기록이 남은 금고를 구버전 앱으로 덮어쓰지 않는다.
