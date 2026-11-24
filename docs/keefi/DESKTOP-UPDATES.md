# KEEFI 데스크톱 업데이트

0.18.1 (112)부터 앱에서 새 배포본을 확인하고 **업데이트 후 재시작**을 선택할 수 있습니다. 0.18.0 (111)과 그 이전 버전에서는 이 기능이 포함된 앱을 한 번 직접 설치하세요. [GitHub Releases](https://github.com/gurosawa/keefi-releases/releases)

현재 테스트 채널은 공개된 서명 목록을 사용하며 사전 릴리스도 표시합니다.

Mac 배포 ZIP은 Intel x86_64용이고 Windows ZIP은 x64용입니다. 배포 파일의 이름, 크기, SHA-256은 KEEFI 배포 키로 서명한 목록에 들어 있습니다. 앱은 내장 공개 키로 이 목록을 확인하고, 선택한 운영체제의 파일이 목록과 일치하는지 검사합니다. 개인 금고와 금융 키를 GitHub에 보내지 않습니다.

Mac에서는 앱 교체를 위해 macOS가 관리자 승인을 요청할 수 있습니다. 이때 입력하는 암호는 Mac 관리자 암호입니다. 업데이트가 끝나면 평소처럼 KEEFI 금고 암호로 기존 계좌를 열 수 있습니다. Windows에서는 ZIP 전체를 전용 폴더에 풀고 현재 사용자가 쓸 수 있는 위치에서 실행하세요. 시스템의 실행 파일 신뢰 정책에 따라 서명·공증되지 않은 앱 실행이 제한될 수 있습니다.

## 배포 담당자

1. Mac과 Windows에서 같은 소스의 조회 전용 배포 ZIP을 만들고, 기존 프로필의 금고가 잠기며 업데이트 후 같은 암호로 계좌를 열 수 있는지 확인합니다.
2. 배포할 두 ZIP을 한 폴더에 놓습니다. 다음 명령은 각 ZIP에서 실제 크기와 SHA-256을 읽어 해당 태그의 `keefi-update.json`을 만듭니다. 버전을 바꿀 때는 빌드 번호도 올립니다.

   ```sh
   python3 scripts/keefi/make-update-manifest.py --directory "$release_dir" --version 0.18.1 --build 112 --tag v0.18.1-rc.112
   ```

3. 배포 Mac의 서명 도구로 목록의 정확한 바이트를 서명합니다. 이 도구는 로그인 키체인의 서비스 `xyz.beamgrove.keefi.release-signing`, 계정 `ed25519-v1`을 사용합니다. `public-key`로 받은 공개 키가 앱에 고정한 키와 일치해야 합니다. 서명 도구의 소스는 `scripts/keefi/sign-update.swift`입니다. 개인 키는 GitHub에 올리지 않습니다.

   ```sh
   "$HOME/Library/Application Support/KEEFI Release Tools/keefi-release-sign" sign-manifest "$release_dir/keefi-update.json" "$release_dir/keefi-update.json.sig"
   ```

4. 검증된 두 ZIP과 목록, 서명 파일을 **같은 태그**의 GitHub Release에 첨부합니다. 모두 올라간 뒤 게시하고 로그아웃한 상태에서도 내려받아 검증하세요. 테스트 배포에는 사전 릴리스 표시를 사용합니다.

업데이트 서명은 KEEFI가 배포한 파일의 무결성을 검증합니다. Apple Developer ID·공증과 Windows Authenticode는 각 운영체제에서 별도로 확인합니다. 키체인의 개인 키를 잃으면 기존 앱이 신뢰하는 업데이트를 서명할 수 없으므로, 신뢰 키 전환은 기존 키를 사용할 수 있을 때 준비해야 합니다.
