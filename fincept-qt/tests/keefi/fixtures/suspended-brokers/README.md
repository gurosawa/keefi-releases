# 비활성 증권사 회귀 시험 자료

Dev 110의 금고와 조회 코드를 사용해 만든 합성 자료다. `credentials.keefi-vault`에는 한국투자증권·LS증권 테스트 계좌가 하나씩 있다. 암호는 `Synthetic suspended brokers 2026!`이다.

`assets-current.keefi`는 위 금고의 원장 키로 암호화한 최신 잔고다. 외부 통신을 차단한 상태에서 `ProviderReadTransport`의 증권사별 합성 응답을 Dev 110으로 조회하고 `checkpointRecords()`로 저장했다. 두 계좌 각각 종목 005930과 KRW 잔고가 들어 있다. 두 파일을 같은 임시 디렉터리에 복사해 이전 버전에서 정상 조회한 계좌가 현재 버전에서 제외되는지 검증한다.
