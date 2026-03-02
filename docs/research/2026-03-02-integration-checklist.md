# 통합 체크리스트 (멀티 에이전트 병렬 작업용)

## 1. AE 파라미터 동기화
- `AeParamValues` 변경 시 `ToRenderParams()` 클램프/매핑을 함께 수정한다.
- 관련 테스트(`tests/test_ae_params.cpp`, `tests/test_ae_router.cpp`)를 동시에 갱신한다.

## 2. Core 파이프라인 동기화
- `RenderPipeline` 입력/출력 상태(enum 포함)를 바꾸면 perf/test 상태 카운트도 같이 갱신한다.
- fallback 단계 추가/변경 시 `tests/test_render_pipeline.cpp`, `tests/perf_harness.cpp` 검증 분기를 맞춘다.

## 3. Inference 인터페이스 동기화
- `IInferenceEngine` 시그니처 변경 시:
  - `ManagedInferenceEngine`
  - `DummyInferenceEngine`
  - `RenderPipeline`
  - 관련 테스트
  를 같은 커밋에서 갱신한다.

## 4. 모델 카탈로그/메뉴 동기화
- `models/models.manifest` 또는 `ModelCatalog` 변경 시 AE 모델 메뉴(`BuildModelMenu`)와 테스트 기본값을 재검증한다.
- 다운로드 스크립트(`tools/download_model.sh`)와 모델 ID 집합이 어긋나지 않게 유지한다.

## 5. 캐시 키 무결성
- 캐시 영향을 주는 파라미터(모델, 품질, 출력 모드 등)를 추가하면 `RenderCacheKey`와 해시 계산, 회귀 테스트를 함께 수정한다.

## 6. 최종 통합 검증
- 최소 검증:
  - `/tmp/zsoda_tests` 통과
  - `/tmp/zsoda_perf_harness --mode benchmark` 통과
  - `/tmp/zsoda_perf_harness --mode stability --frames 1000` 통과
