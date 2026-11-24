# Dev99가 만든 합성 금고

이 폴더의 `credentials.finix-vault`는 Dev100 이름 변경 전에, Dev99 빌드에 사용한 `libfinix_vault.a`를 직접 링크한 별도 생성 프로그램으로 만들었다. 새 후보의 `Vault.cpp`로 다시 생성한 파일이 아니다. 실제 금고·계좌·키·프로필을 읽거나 실제 앱을 실행하지 않았다.

## 생성 증거

- 생성 시각: `2026-11-13T09:48:47.568Z`
- 당시 소스 HEAD: `cef32c29c709b0f127c4fa118ef7e9617dcd5d05` (Dev99 설치 영수증)
- 컴파일러: `/Library/Developer/CommandLineTools/usr/bin/c++`, Apple clang `17.0.0 (clang-1700.6.3.2)`, 대상 `x86_64-apple-darwin25.6.0`
- Qt: `6.8.3`, SDK: `MacOSX26.2.sdk`, 최소 macOS: `13.0`
- 구 라이브러리: 작업 폴더의 `build/libfinix_vault.a`
- 구 라이브러리 SHA-256: `5fc35dac2224dd99e03e7831c727852b56ea1c14862a994d9e68f824c1a08340`
- 구 `fincept-qt/src/finix/Vault.cpp` SHA-256: `f4eb62c8108633e80415e9dd0675bfc7abbeaff9a3c794e34478c63effe8530e`
- 생성한 금고: **760바이트**, SHA-256 `2e136e637980c02d9e3499b32452344faa2b722494e44a6ebf68eb24d01e85e6`
- `generate.cpp` SHA-256: `aac72c978cf82f7be737521fdbe38d6e54ac650ef22b4e83f51acf99a45fcf27`
- 공개 보존본 `generate.sh` SHA-256: `4d1ac82fd6523215876b4a00218b231d254b3282ae8031252c757f83f03a987d` (Qt와 libsodium 설치 경로는 환경 변수로 지정)

생성 직전과 직후 구 라이브러리·Vault.cpp 해시가 같음을 다시 확인했다. 생성기는 두 해시와 위 HEAD 인자가 다르면 실패하며, 기존 금고 또는 manifest를 덮어쓰지 않는다. 컴파일·링크 명령은 `generate.sh`에 보존했으며, 공개 보존본은 Qt와 libsodium의 설치 경로를 각각 `QT_PREFIX`와 `SODIUM_PREFIX`에서 읽는다. 이 명령은 `Vault.cpp`를 컴파일하지 않고 기록된 정적 라이브러리를 링크한다.

소스 작업 트리에서 실제 실행한 명령:

```sh
/bin/bash fincept-qt/tests/finix/fixtures/dev99-profile/generate.sh > ../tools/dev100-legacy-fixture-build-and-run.out 2>&1
```

종료 코드는 0이며 로그는 다음과 같다.

```text
generated synthetic legacy vault 760 bytes; sha256 2e136e637980c02d9e3499b32452344faa2b722494e44a6ebf68eb24d01e85e6
```

## 합성 내용과 확인

암호는 **`FINIX synthetic Dev99 password 2026!`**다. 전부 시험을 위해 정한 값이다. `manifest.json`에 계좌 식별자·표시 이름·서비스·시장과 해시를 함께 기록했다.

생성기는 `QTemporaryDir` 안에서 구 Vault의 `create()`를 호출하고, 고정 가짜 Upbit 계좌 한 개와 가짜 키를 추가했다. 별도 자산 기록용 32바이트 키도 구 API로 만들었으며 원시 키는 출력하지 않았다. 이어서 `lock()`을 호출하고 암호문을 읽었다. 구 구현으로 같은 암호를 다시 열어 계좌 식별자·표시 이름·기록 키의 존재를 확인한 뒤 다시 잠갔으며, 재열기 전후 파일 전체 바이트가 같음을 확인했다. 암호문 안에 가짜 계좌 표시 이름과 가짜 키의 평문이 없다는 확인도 통과했다.

네트워크 객체와 요청은 없다. `FINIXVLT` 헤더와 파일 이름은 그대로다. 이 fixture는 금고 파일이며 다른 기록 저장소를 만들었다는 의미는 아니다.

후속 시험은 이 파일을 읽어 **새 임시 폴더의 사용자 전용 파일로 복사한 뒤** 새 후보 Vault로 열어야 한다. 저장소 원본 fixture에 직접 Vault를 연결하거나 덮어쓰지 않는다. Git은 사용자 전용 권한을 보장하지 않으므로 임시 사본에 `0600`, 임시 프로필 폴더에 `0700`을 명시한다. 성공/실패/잠금 전후 전체 바이트와 해시를 비교한다. 암호화의 난수 때문에 새로 생성한 금고는 같은 합성 입력이라도 바이트가 다르므로, 이 보존 파일의 해시가 호환성 기준이다.
