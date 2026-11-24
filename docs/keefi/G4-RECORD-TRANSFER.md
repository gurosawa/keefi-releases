# KEEFI 기록 이동 — 선택적 암호화 기록 내보내기·가져오기

자산과 관찰 기록을 선택해 암호화 파일로 내보내고 별도의 로컬 저장소로 가져오는 기능이다. 금융 키·금고·시드를 옮기거나 현재 계좌에 자동 병합하지 않는다. 현재 파일명과 암호화 식별자는 [저장 형식](VAULT-FORMAT.md)의 KEEFI 계약을 따른다. 아래의 형식·상한·복구 경계를 함께 확인한다.

## 자료와 제외

사용자가 자산 스냅샷, 자산 사건, 주문 관찰, 스왑 관찰, 금고의 미확정 요청 관찰을 각각 선택한다. 원장 본체와 모든 archive를 읽는다. 원장의 시세 배열, assets-current 캐시, 원시 시세, 공유 가격 캐시는 내보내지 않는다. 성과는 입력 기록에서 계산되는 값이며 독립 성과 자료는 없다. 수동 보정 및 지속 비민감 설정은 현재 구현되지 않았으므로 이번 형식에서 지원한다고 표시하지 않는다.

금융 자격 증명 금고/키/Secret/토큰, 개인키/시드, 일회성 세션, 서명된 transaction bytes, connectionBinding, credential fingerprint를 내보내지 않는다. 포괄 JSON 복사 대신 각 종류의 필드 allowlist를 새로 만든다. 원본 credentialRef/connectionId는 금융 연결 생성에 사용하지 않는다. 원본 자산 scope가 credential fingerprint일 수 있으므로 portable scope는 SHA-256(domain, 출처의 랜덤 origin UUID, 원본 scope)의 고정 별칭으로 변환한다. 그 관계만 보존하고 원문 scope를 노출하지 않는다. 공식 주문/체결/transaction ID와 원본 시각·환경·시장·완전성은 보존한다. evidence의 자유 scope/coverage는 원래 인증 범위를 포함할 수 있어 scope 별칭 및 명시된 기록 범위로 대체한다. 원문 기록 ID가 fingerprint를 포함하는 자산 사건 id 대신 공식 사건 ID+종류+자산으로 동일성을 정의한다.

각 기록은 origin UUID, kind, identity, payload를 갖는다. 종류는 snapshot/event/order/swap/pending이다. origin은 명시적 초기 준비 때 랜덤 생성해 로컬 암호화 store에 보존한다. imported 자료의 origin과 portable identity는 재export 때 바꾸지 않는다. 환경을 알 수 없는 레거시 관찰은 unknown으로 보존하며 현재 계좌로 추정 연결하지 않는다.

## 패키지

압축·디렉터리·임의 파일을 포함하지 않는 단일 암호화 파일이다. 정수는 little endian. 72바이트 헤더: magic KEEFIRTX(8), version=1(u16), KDF=1(u16), cipher=1(u16), reserved=0(u16), ops(u32), memory bytes(u64), salt(16), nonce(24), ciphertext+tag 길이(u32). 전체 헤더를 AAD로 인증한다. Argon2id13, XChaCha20-Poly1305이며 새 export마다 salt와 nonce를 생성한다. 기본 비용 ops=3/memory=64MiB, 허용 ops=3..6/memory=64..256MiB. 새 암호는 Vault::validateNewPassword 정책을 따른다. import에는 새 복잡도 정책을 소급하지 않는다.

평문은 Qt compact canonical JSON {schema_version:1,records:[...]}. 알 수 없는 필드·중복 key 표현·비canonical JSON·종류·누락 필드·잘못된 자료형을 거부한다. 파일 최대 64MiB, 기록 최대 100000, 단일 일반 관찰 기록 256KiB, 자산 snapshot 8MiB(원본 components 최대 2048개 유지), 문자열 4096자, 배열별 상한을 검사한다. KDF 전 헤더/비용/길이, JSON parse 전 평문 크기, merge 전 record 수를 확인한다. 상한 초과·손상된 archive를 생략해 성공하지 않는다. 암호/키는 SecureBuffer에서 최소 보관하며 평문 임시 파일은 만들지 않는다.

## 목적지 store와 commit

별도 imported-records.keefi 한 파일만 사용한다. magic KEEFIIMP, format=1/reserved=0, nonce 24바이트, ciphertext 길이의 40바이트 헤더 전체를 AAD로 인증한다. 로컬 Vault 랜덤 기록키에서 KEEFI/imported-records/v1 domain으로 키를 유도한다. 평문은 canonical JSON {schema_version:1,origin:UUID,records:[...]}다. 기존 Vault, assets/current/history, swap journal, pending_operations, 계좌를 import로 변경하지 않는다. 필요한 기록키와 빈 store는 별도 명시적 initialize 단계에서 생성한다. initialize는 기존 store를 덮어쓰지 않는다.

import는 전체 복호화/스키마 검증 후 기존 store와 메모리에서 merge한다. origin/kind/identity가 같고 내용도 같으면 duplicate이며 파일을 다시 쓰지 않는다. 같은 identity의 내용이 다르면 전체 import를 거부한다. swap/pending은 원본 요청 ID와 관찰 revision(state 및 확인 결과/작업 ID의 canonical hash)을 identity로 사용한다. 정상 후속 상태와 결과 보충은 별도 관찰 row로 추가하며 과거 row를 덮어쓰지 않는다. 같은 origin/종류/원본 요청 ID의 수량·방향·서명 대상 등 불변 의도가 다르거나 이미 확인된 주문 ID·slot·수수료·수량이 모순되면 revision이 달라도 전체 import를 거부한다. 과거 package를 나중에 다시 가져와도 기존 관찰을 후퇴시키지 않는다. 원래 자료와 무관한 source origin 사이를 자동 합산/대사하지 않는다. 파일명과 원본 ID를 경로로 사용하지 않는다.

로컬 store commit은 owner-only 디렉터리/파일, 심볼릭 링크/하드 링크 금지, 파일 잠금, 기존 digest 검사, 암호문만 임시 파일 기록, fsync, 복호화 readback, 잠금 generation/취소/digest 재검사, 단 한 번 atomic rename, rename 직전 취소/generation 재확인, 디렉터리 fsync와 최종 readback 순서다. 사용자 지정 이동 패키지 I/O는 별도 정책이다. 다른 PC에서 복사한 암호문은 소유자·기존 mode를 강제하지 않고 regular file/no symlink/no hardlink, 열린 FD의 종류·크기와 형식을 검증한다. export는 일반 Documents/Desktop/이동 폴더에도 새 0600 파일만 생성하고 기존 파일을 덮어쓰지 않는다. 부모 폴더 권한을 변경하지 않는다. 암호문 temp·원자 생성·readback은 유지한다. 로컬 Vault/store/자산 파일의 엄격 정책은 유지한다. 교체 전 실패는 기존 bytes를 보존한다. rename 뒤 fsync/readback 실패는 완료를 주장하지 않고 store를 잠근다. 프로세스 중단 뒤 old/new 완전 파일 중 하나만 읽는다.

## Qt thread와 실행 금지

Vault/AssetsLedger/SwapJournal과 RecordTransfer instance의 initialize/open/collect/commit은 owner Qt thread에서만 호출한다. 부모가 export 전 pendingHistory timer와 비동기 원장 쓰기의 checkpoint를 완료한다. shared ledger/journal이 열려 있으면 그 instance를 사용하며 동일 파일을 다시 열지 않는다. static codec 및 암호문 파일 읽기/쓰기는 immutable bundle과 이동한 SecureBuffer만 받아 worker에서 실행할 수 있다. worker는 Vault/QObject에 접근하지 않는다. 결과 commit에는 시작 generation과 취소 token을 확인한다. 금고 잠금은 즉시 store key/평문을 지우고 generation을 바꾼다.

imported 주문/미확정/스왑은 관찰용이다. 컨트롤러 pending으로 넣지 않고 서명/전송/재전송/주문 권한 활성화를 하지 않는다. 현재 상태는 사용자가 별도 등록한 금융 키로 다시 조회한다. 같은 프로필의 중복 실행 잠금은 서로 다른 PC·프로필의 조회나 기록 이동을 조정하는 분산 잠금이 아니다. 목적지 PC에서는 공식 현재 자료를 별도로 조회한다. 패키지 암호와 로컬 기록키를 잃으면 운영자 복구는 없다.

## 검증 범위

과거 Dev101 기준 자동 검사에는 기록 codec·저장·왕복, 중복·충돌·변조·취소·잠금, 기록 이동 UI 등의 합성 검사가 포함됐다. 당시 실행과 현재 후보의 증거 범위는 [검증 상태](VERIFICATION-STATUS.md)에서 구분한다. 개인 금고·실제 인증정보·사용자 자료를 자동 검사의 입력으로 삼지 않는다.

합성 프로필에서 저장 중단·실패를 주입하는 검사는 실제 물리 디스크 고갈·전원 차단, 다른 물리 PC/OS 또는 실제 사용자 자료 복원의 증거가 아니다. 가져온 기록은 현재 자산·성과 합계나 주문 대기열에 자동 편입하지 않는다.

## 사용자 절차와 복구 한계

1. 원본 PC에서 금고를 열고 **기록 이동 → 기록 이동 준비**를 누른다. 창을 열기만 해서는 기록 키나 저장소를 만들지 않는다.
2. 내보낼 종류를 선택하고 새 `.keefirtx` 경로와 패키지 암호·암호 확인을 입력한 뒤 **선택한 기록 내보내기**를 누른다. 암호는 금고 암호와 별도로 정하며 완료 개수와 수집 범위 상세를 확인한다. 같은 파일을 덮어쓰지 않는다.
3. 패키지를 사용자가 직접 이동한다. 목적지 PC에서 별도의 로컬 금고를 만든 뒤 **기록 이동 준비 → 가져오기**에서 파일과 패키지 암호를 입력하고 **검증 후 기록 가져오기**를 누른다.
4. 추가·중복 개수를 확인하고 **가져온 기록 보기**에서 읽는다. 현재 자산·성과 합계, 금융 연결, 주문 대기열에는 자동 편입하지 않는다. 금융 키는 목적지 PC에 별도 등록하고 공식 현재 잔고·주문을 새로 조회한다.

이미 내보낸 패키지가 있어야 새 금고에서 기록을 복구할 수 있다. 패키지 암호를 잃으면 운영자 복구 수단은 없으며 금융 키 복구는 지원하지 않는다. 취소가 원자 저장 직후 도착하면 새 암호화 패키지가 남을 수 있어 완료 후 파일 존재 여부를 안내하며 자동 삭제하지 않는다. 가져온 기록의 출처가 다르면 자동 합산하지 않는다.
