# KEEFI 라이선스와 원본 고지

2026-10-18 확인. KEEFI는 Fincept Terminal에서 파생한 **AGPL-3.0-or-later** 프로젝트다. 최신 공식 안내의 기준은 upstream `main`의 커밋 `3d8617cae3f13c93874d4bee4fb0f2846e9c83bc`에 있는 [Licensing v3.0](https://github.com/Fincept-Corporation/FinceptTerminal/blob/3d8617cae3f13c93874d4bee4fb0f2846e9c83bc/docs/COMMERCIAL_LICENSE.md)이다. 이 안내는 2026-08-11부터 적용되며 AGPL 단일 라이선스와 이전 Commercial License v2.0의 전면 철회를 명시한다.

이전에 G5 문서에서 ‘라이선스 충돌 미해결’로 남긴 판단을 정정한다. 철회된 상업 라이선스 조건을 별도 허가·구매·문의가 필요한 공개 차단 사유로 사용하지 않는다. 원본 고지와 이력은 보존하며 KEEFI에 새로운 제한을 추가하지 않는다.

## 공개본에 적용하는 사항

- KEEFI 수정 소스와 빌드 절차를 같은 저장소에서 제공하고 AGPL-3.0-or-later를 유지한다. 바이너리를 별도로 배포할 때도 그 버전에 대응하는 전체 소스와 필요한 빌드 자료를 함께 제공한다.
- 원저작권자 Fincept Corporation과 기여자 고지를 유지한다. KEEFI는 독립된 파생 제품이며 Fincept의 공식 배포·보증으로 표시하지 않는다.
- 제품명은 KEEFI / KEEFI Dev를 사용한다. 원본 이름은 출처·저작권 설명에 남긴다. 상표권을 함께 양도받았다고 주장하지 않는다.
- 각 의존성의 라이선스와 고지는 별도로 유지한다. [의존성 고지](THIRD-PARTY-NOTICES.md)를 참조한다.
- 초기 저장소의 [MIT 파일](INITIAL_REPOSITORY_LICENSE.txt)은 당시 이력이다. 현재 전체 소스의 라이선스를 MIT로 바꾸는 선언이 아니다.

현재 첫 베타는 GitHub **소스 공개** 준비다. Apple Developer ID·공증, 별도 도메인, 유료 상업 라이선스 구매를 준비 조건으로 두지 않는다. 실제 공개 여부와 공개할 커밋은 소유자가 결정한다.

## 근거와 보존

[원본 라이선스 안내](../COMMERCIAL_LICENSE.md)의 v3.0 내용을 기준으로 위 사항을 정리했다. 이전 루트 LICENSE의 추가 제한 문구는 최신 안내를 설명하는 근거로 사용하지 않는다. 루트 [LICENSE](../../LICENSE)에는 AGPL 원문을 제공하고, 인계받은 이전 루트 파일은 [upstream/LICENSE.txt](upstream/LICENSE.txt)에 출처 기록으로 보존한다. 이는 AGPL에서 다른 라이선스로 변경하는 작업이 아니라, 최신 공식 선언에 맞춘 공개 문서 정리다.

AGPL 원문은 [SPDX license-list-data v3.27.0의 AGPL-3.0-or-later](https://github.com/spdx/license-list-data/blob/v3.27.0/text/AGPL-3.0-or-later.txt)를 변경 없이 사용했다. SHA-256은 `d8a6cc31abc16b6748c7a21f21611f5a1ec33f67d22ca23d7da1c19b95496bee`다. 제품·원본 저작권은 루트 [NOTICE](../../NOTICE)에 함께 기록한다.
