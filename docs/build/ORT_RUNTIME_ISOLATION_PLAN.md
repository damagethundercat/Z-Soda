# ORT Runtime Isolation Plan (Adobe AE DLL 충돌 방지)

본 문서는 After Effects 프로세스 내 ORT DLL 충돌을 구조적으로 방지하기 위한 설계/검증/롤아웃 계획이다.

## 1) 문제 요약

- 현재 충돌 시나리오:
  - AE 프로세스가 Adobe 번들 `onnxruntime.dll`(1.17.x)을 먼저 로드
  - ZSoda 플러그인은 1.24.x(예: 1.24.2) 헤더/라이브러리 기준으로 빌드됨
- 관측 증상:
  - `The requested API version [24] is not available ... [1, 17]`
  - 초기화 실패 후 접근 위반(`C0000005`) 또는 플러그인 로드 불안정
- 원인 핵심:
  - Windows DLL 검색 순서 + 암시적(implicit) 링크가 결합되어, 의도한 런타임 대신 프로세스 내 선행 ORT 모듈을 재사용함

## 2) 해결 원칙

1. implicit link 제거
- 플러그인 바이너리 import table에서 `onnxruntime.dll` 의존을 제거한다.
- ORT 심볼 직접 링크 대신 `LoadLibrary` + `GetProcAddress` 기반 초기화로 전환한다.

2. explicit `LoadLibrary` 절대경로 강제
- 플러그인 모듈 경로 기준으로 ORT DLL의 절대경로를 조합한다.
- 예시: `<PluginDir>\runtime\onnxruntime.dll` (또는 배포 정책상 동등한 고정 위치)
- `LoadLibraryExW(abs_path, ..., LOAD_WITH_ALTERED_SEARCH_PATH)`로 대상 파일만 로드한다.

3. API 버전 협상(negotiation) 필수화
- `OrtGetApiBase` 획득 후 `GetApi(ORT_API_VERSION)` 호출 결과를 검사한다.
- 실패 시 즉시 추론 파이프라인을 비활성화하고 안전 fallback으로 전환한다.
- 성공 시 로드된 DLL 버전/경로/API 버전을 1회 로그로 남긴다.

## 3) 제안 설계

### 3.1 로더 계층

- `OrtRuntimeLoader`(단일 책임):
  - DLL 절대경로 계산
  - 동적 로드/심볼 해석
  - API 협상
  - 진단 정보 캡처(경로, 파일 버전, 협상 결과)
- `InferenceRuntime`는 `OrtRuntimeLoader` 결과만 소비하고 직접 DLL 로딩을 하지 않는다.

### 3.2 초기화 흐름 (요약)

1. 플러그인 시작 시(또는 모델/백엔드 설정 변경 시) `InitializeOrtRuntime()` 호출
2. 후보 경로 계산:
   - 1순위: `<PluginDir>\runtime\onnxruntime.dll`
   - 2순위: 구성값으로 지정된 절대경로(옵션)
3. `LoadLibraryExW` 성공 시 `GetProcAddress("OrtGetApiBase")`
4. `GetApi(ORT_API_VERSION)` 호출 결과 검사
5. 실패 분기:
   - ORT 초기화 실패 상태 플래그 설정
   - 안전 출력(패스스루/빈 깊이맵)으로 계속 렌더
6. 성공 분기:
   - 세션/캐시 초기화 진행
   - 로드 경로/버전/API를 rate-limited 로그로 기록

### 3.3 빌드/패키징 변경

- 빌드:
  - Windows에서 `onnxruntime.lib`를 직접 링크하지 않는 옵션을 기본값으로 전환
  - 동적 로딩 전용 모드(`ZSODA_ORT_EXPLICIT_LOAD=ON`)를 기본 정책으로 사용
- 패키징:
  - `ZSoda.aex`와 동일 배포 단위에 `runtime/onnxruntime.dll` 포함
  - Adobe/시스템 경로 DLL 덮어쓰기 방식은 금지

### 3.4 로그/진단 항목 (최소)

- `ORT module path`: 실제 로드된 DLL 절대경로
- `ORT file version`: 파일 버전(예: 1.24.2)
- `ORT api request/result`: 요청 API 버전/협상 결과
- `fallback reason`: 로드 실패, 심볼 실패, API 협상 실패 등

## 4) 검증 체크리스트

1. 모듈 경로 검증
- AE 프로세스에서 ORT 모듈이 플러그인 배포 경로(`...ZSoda...\runtime\onnxruntime.dll`)로 로드되는지 확인
- Adobe/시스템 경로 ORT가 사용되지 않는지 확인

2. 버전/협상 검증
- 로그에 `file version=1.24.x`, `requested_api=24`, `negotiation=success`가 남는지 확인
- 강제 구버전 DLL 주입 시 `negotiation=fail` + fallback 로그가 남는지 확인

3. fallback 동작 검증
- ORT 로드 실패 시:
  - AE 크래시 없이 이펙트가 안전 출력으로 동작
  - 렌더 루프가 중단되지 않음
  - 에러 로그가 과다 반복되지 않음(rate limit)

4. 회귀 검증
- ST-01/ST-03/ST-04/ST-06 스모크 테스트 통과
- 스크럽/프리뷰/렌더 큐에서 초기화 재진입 및 쓰레드 안전성 확인

## 5) 단계별 롤아웃

### Phase 0 - 관측성 확보

- 기존 코드에 "실제 로드된 ORT 경로/버전/API 협상 결과" 로그만 우선 추가
- 현장 재현 로그를 수집해 기준선(baseline) 확정

### Phase 1 - 명시적 로더 도입 (옵트인)

- `ZSODA_ORT_EXPLICIT_LOAD=ON` 빌드 플래그 추가
- CI/로컬에서 opt-in 경로로 기능 검증
- 실패 시 기존 경로로 빠르게 되돌릴 수 있도록 가드 유지

### Phase 2 - 기본값 전환

- explicit 로딩을 기본값으로 변경
- implicit 링크 경로는 비권장(legacy)로 유지하되 릴리스 노트에 제거 예고

### Phase 3 - implicit 경로 제거

- import table 기반 ORT 의존 완전 제거
- 문서/스크립트/패키징을 explicit 정책으로 단일화

## 6) 수용 기준 (Done)

- `onnxruntime.dll` 충돌 재현 환경에서 AE 크래시 없이 fallback 동작
- 정상 환경에서 ORT 1.24.x 경로/버전/API 협상 성공 로그 확인
- Windows 스모크 테스트(ST-01/ST-03/ST-04/ST-06) 통과
- `docs/build/LOCAL_AGENT_HANDOFF.md`, `docs/build/README.md`에 운영 가이드 반영 완료
