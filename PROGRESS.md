# 진행 현황 (PROGRESS)

이 문서는 Z-Soda(After Effects 플러그인) 개발의 현재 상태를 요약합니다.  
깨진 인코딩(모지바케) 이슈를 정리하여 UTF-8 한글 기준으로 재작성했습니다.

## 1) 전체 진행률
- 전체 진행률: **92%**
- 기준 시점: **2026-03-05**
- 현재 포커스:  
  1. 멀티뷰 선택값이 실제 렌더 경로에 반영되지 않는 문제  
  2. AE 로더에서 `ZSoda.aex`가 `Ignore`로 분류되는 간헐 이슈

## 2) 단계별 상태 (PLAN.md 기준)
- [x] `P1` 플러그인 기본 구조 및 AE SDK 핸들러
- [x] `P2` 모델/세션 생명주기 + 캐시 기반 렌더 파이프라인
- [~] `P3` Depth Map/Slicing + 8/16/32bpc 경계 처리 (대부분 완료, 호스트 파라미터 반영 안정화 진행 중)
- [~] `P4` 실패 대응 체인(OOM/백엔드 실패/다운스케일/안전 출력) (운영 안정화 단계)
- [~] `P5` 테스트/성능/패키징/배포 자동화 (Windows 경로 고도화 완료, AE 실전 시나리오 QA 진행 중)

## 3) 최근 완료 작업
- [x] `D139` DA3 Multi-View 전환: VDA 옵션 정리, DA3 멀티뷰 입력 경로 추가
- [x] `D140` ORT 출력 텐서 선택 안정화: depth 출력 인덱스/이름 선택 및 진단 로그 강화
- [x] `D141` 실제 멀티뷰 모델 항목(`depth-anything-v3-small-multiview`) 추가 및 진단 확장
- [x] `D142` 빌드/배포 완료: MediaCore 배포 + ORT/모델 자산 동기화
- [x] `D143` Extract 강제 재추론 경로 보강: `extract_token`을 캐시 키에 반영
- [x] `D144` 멀티뷰 파라미터 전파 보강: `num_params=1` 같은 축소 힌트에서도 파라미터 테이블 fallback 읽기
- [x] `D145` AE 파라미터 감시 강화: 주요 파라미터에 `PF_ParamFlag_SUPERVISE` 적용
- [x] PROGRESS 문서 인코딩 복구: 깨진 한글 문서 전면 정리

## 4) 현재 확인된 이슈
1. 멀티뷰가 사용자 기대대로 동작하지 않음  
   - 최신 로그에서 반복적으로 `model=depth-anything-v3-small`, `frames=1`, `requested_multiview=0` 확인
2. AE 플러그인 로더 이슈  
   - `Plugin Loading.log`에 `No loaders recognized this plugin, so the plugin is set to Ignore` 기록
   - 이 경우 최신 빌드가 실제 렌더링에 반영되지 않음

## 5) 다음 작업 우선순위
1. AE 로더 Ignore 상태 재현/해결  
   - 로더 캐시/플러그인 메타 검증 루틴 강화
2. 멀티뷰 활성화 강제 검증  
   - `small-multiview` 선택 시 런타임에서 `requested_multiview=1`, `frames>=5`가 아니면 명시 경고
3. 품질 격차(Quick Depth 3 대비) 축소용 HQ 경로 설계  
   - 클립 단위 정규화/temporal 안정화/edge-aware 정제 강화

## 6) 배포 기준 정보(최근)
- 최근 배포 AEX: `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\ZSoda.aex`
- 최근 배포 해시(SHA256): `5ba56896b828f028da80e31e9d1ab11478a0adc93b59ec48dc3eada9bb48ac55`

## 7) 리스크
- AE 호스트의 로더/캐시 정책 변화에 따라 동일 빌드라도 로드 여부가 달라질 수 있음
- ORT 공존 환경(Adobe 내장 ORT + 플러그인 ORT)에서 초기화/선택 정책 불안정 시 품질/안정성 이슈 재발 가능

## 8) 다음 공유 시점
- 로더 Ignore 이슈 원인 고정 + 멀티뷰 활성 로그(`requested_multiview=1`, `frames=5`) 확보 직후
