# KEEFI 보안 정책

## 지원 범위

보안 수정의 대상은 최신 공개 KEEFI 소스의 **금융 조회 전용 빌드**입니다. 확인한 플랫폼과 자동 검사·실제 계좌 확인의 차이는 [검증 상태](docs/keefi/VERIFICATION-STATUS.md)를 따릅니다.

## 비공개 신고

[KEEFI 공개 저장소](https://github.com/gurosawa/keefi-releases)에서 보안 문제를 신고할 수 있습니다.

신고 채널은 **GitHub 비공개 취약점 신고(Private vulnerability reporting)**입니다.

기능이 활성화된 공개 저장소에서 `Security and quality → Report a vulnerability`를 선택해 비공개로 신고하세요. [GitHub 공식 신고 안내](https://docs.github.com/en/code-security/how-tos/report-and-fix-vulnerabilities/report-privately)를 따릅니다.

신고 버튼이 보이지 않으면 취약점 상세나 재현 자료를 공개 이슈에 올리지 마세요. 상세 내용 없이 비공개 신고 경로만 문의하고, 소유자가 비공개 경로를 안내한 뒤 내용을 전달하세요. 확인되지 않은 이메일 주소로 보내지 마세요.

## 신고에 포함할 내용

사용한 소스 버전 또는 커밋, 운영체제 버전, 예상 영향, **가짜 키·합성 자료로 재현하는 최소 절차**를 적어 주세요. 일반 버그는 [버그 양식](.github/ISSUE_TEMPLATE/bug_report.md)을 사용합니다.

**보내면 안 되는 자료:** 금융 키·Secret·토큰·인증 헤더·금고 암호·시드·개인키·원본 인증 응답·개인 금고나 프로필·실제 금융 원장·잔고 또는 계좌 화면. 비공개 신고에도 이런 자료를 첨부하지 마세요. 실제 계좌나 다른 사람의 자료를 사용하는 재현은 요구하지 않습니다.

## 데이터 보호의 한계

금고 암호화, 화면 잠금, 조회 전용 요청 경계는 서로 다른 보호 기능입니다. 저장 암호화만으로 운영체제 침해나 잠금 해제 상태의 모든 메모리·화면 노출을 막는다고 보장하지 않습니다. 구현과 검증의 범위는 [저장 형식](docs/keefi/VAULT-FORMAT.md)과 [검증 상태](docs/keefi/VERIFICATION-STATUS.md)를 확인하세요.
