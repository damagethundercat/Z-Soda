# 2026-03-05 Depth Quality A/B 실험 설계서 (Z-Soda vs Quick Depth 3)

## 1) 목적
- 현재 보고된 품질 이슈(타일 경계, 배경 거칠기, 엣지 뭉개짐)의 **주 원인 경로**를 분리 진단한다.
- 모델 자체 성능 비교가 아니라, 실행 파이프라인(타일 합성/리파인/후처리/멀티뷰)의 영향을 정량화한다.
- 코드 수정 전, 우선순위가 높은 개선 포인트를 데이터 기반으로 확정한다.

## 2) 분석 기준(현재 코드 사실)
- Detail Boost 기본 활성 조건:
  - `quality >= 3 && min_extent >= 720`이면 기본 ON
  - 근거: `plugin/core/RenderPipeline.cpp` `ShouldEnableDetailBoost(...)`
- Direct 성공 후에도 Detail Boost에서 타일 재추론 수행:
  - 근거: `plugin/core/RenderPipeline.cpp` `Render(...)` -> `ApplyDetailBoostRefinement(...)`
- 일반 tiled fallback은 temporal/multiview 모델에서 skip 가드 존재:
  - 근거: `plugin/core/RenderPipeline.cpp` `IsTemporalSequenceModelId(...)`, `skip_tiled_fallback`
- Detail Boost 경로에는 동일 temporal skip 가드가 없음:
  - 근거: `plugin/core/RenderPipeline.cpp` `ApplyDetailBoostRefinement(...)` 내부
- 타일 합성은 겹침 영역 균일 평균(weight=1) 방식:
  - 근거: `plugin/core/Tiler.cpp` `ComposeTiles(...)`
- AE 파라미터 변환에서 모델 ID는 locked path가 기본:
  - 근거: `plugin/ae/AeParams.cpp` `ToRenderParams(...)`, `ResolveLockedModelId()`

## 3) 가설
- H1. 타일 seam은 `ComposeTiles`의 균일 평균 합성으로 발생/강화된다.
- H2. Detail Boost 재타일링이 seam과 거친 그라데이션을 증폭한다.
- H3. temporal/multiview에서 Detail Boost 타일링이 frame-hash 기반 히스토리와 충돌할 수 있다.
- H4. 후처리 매핑(특히 quantile/guided 계열)과 타일 재추론이 결합되어 배경이 거칠어진다.
- H5. UI 모델 선택과 실제 추론 모델이 불일치하면 비교 실험 결과가 왜곡된다.

## 4) 공통 통제 조건
- 프로젝트/클립:
  - 동일 소스, 동일 프레임 범위, 동일 해상도
  - 최소 2개 시퀀스: `정적 배경+미세 엣지`, `빠른 모션+복잡한 배경`
- 렌더 설정:
  - 동일 bit depth(권장 32bpc float), 동일 색공간 설정(Linear on/off는 별도 실험으로 분리)
- 플러그인 입력:
  - Z-Soda/Quick Depth 3 모두 동일 위치/마스크/트랜스폼
- 기록:
  - `%TEMP%\ZSoda_AE_Runtime.log`
  - `%TEMP%\BSKL\quickdepth3_render-engine.log`

## 5) 실험 매트릭스

### Phase A: Z-Soda 내부 원인 분리 (필수)

#### A0. Baseline
- 설정: 현재 기본값
- 기대: 현재 문제 재현

#### A1. Detail Boost 영향 분리
- 설정:
  - `ZSODA_DETAIL_BOOST=0`
  - 비교군으로 `ZSODA_DETAIL_BOOST=1`
- 판정:
  - seam/거칠기 지표가 A1(OFF)에서 크게 감소하면 H2 강하게 지지

#### A2. Detail Boost 타일 파라미터 민감도
- 설정 (Detail Boost ON 고정):
  - `ZSODA_DETAIL_BOOST_TILE_SIZE`: `384`, `608`, `960`
  - `ZSODA_DETAIL_BOOST_OVERLAP`: `32`, `76`, `128`
  - `ZSODA_DETAIL_BOOST_BLEND`: `0.15`, `0.35`, `0.55`
- 판정:
  - tile/overlap에 따라 seam 패턴이 이동/증감하면 H1/H2 지지

#### A3. 매핑/후처리 영향 분리
- 설정:
  - `ZSODA_DEPTH_MAPPING_MODE=raw|normalize|guided`
  - `ZSODA_EDGE_ENHANCEMENT=0.0|0.03|0.08`
  - `ZSODA_EDGE_AWARE_UPSAMPLE=true|false`
- 판정:
  - 배경 거칠기/엣지 선명도의 트레이드오프를 수치화해 H4 검증

### Phase B: Temporal/Multiview 리스크 검증 (필수)

#### B1. 단일 프레임 기준선
- 설정:
  - `ZSODA_DA3_MULTIVIEW_FRAMES=1`

#### B2. 멀티뷰 강제
- 설정:
  - `ZSODA_LOCKED_MODEL_ID=depth-anything-v3-large-multiview`
  - `ZSODA_DA3_MULTIVIEW_FRAMES=5`
- 실험:
  - B2a: `ZSODA_DETAIL_BOOST=0`
  - B2b: `ZSODA_DETAIL_BOOST=1`
- 판정:
  - B2b에서만 지터/오류 증가 시 H3 지지
  - 로그에서 `temporal_history_reuse_duplicate_frame` 빈도 확인

### Phase C: Quick Depth 3 비교 (블랙박스 비교)

#### C0. 동일 입력/동일 프레임셋 비교
- Quick Depth 3와 Z-Soda를 동일 컴포지션에서 렌더
- 출력 포맷 통일(권장 EXR/PNG 시퀀스)

#### C1. 런타임 스택 차이 기록
- Quick Depth 로그에서 `Using CUDA`, `Using float16` 확인
- Z-Soda 로그에서 backend/provider/입력 shape/멀티뷰 상태 확인

## 6) 측정 지표

### M1. Seam Score (낮을수록 좋음)
- 타일 경계 예상선 주변(수직/수평)의 depth 불연속 절대값 평균.
- 실험 간 상대 비교 중심으로 사용.

### M2. Background Smoothness (높을수록 좋음)
- 배경 ROI에서 Laplacian 분산의 역수(또는 고주파 에너지 역수).
- 거칠기/노이즈 체감과 높은 상관.

### M3. Edge Preservation (높을수록 좋음)
- 객체 경계 ROI에서 depth gradient magnitude 평균.
- 턱선/날개 등 얇은 구조물의 보존력 평가.

### M4. Temporal Stability (높을수록 좋음)
- 고정 ROI 픽셀들의 프레임간 표준편차 역수.
- 멀티뷰/시퀀스에서 지터 평가.

### M5. Throughput (높을수록 좋음)
- 프레임당 처리 시간(ms) 평균/중앙값.
- 로그 타임스탬프 기반 추정.

## 7) 로그 수집 포인트
- Z-Soda:
  - `run_prepare_input_ok`
  - `run_exit_onnx`
  - `render_after_pipeline` 메시지(`detail boost applied ...`)
  - `temporal_history_reuse_duplicate_frame`
- Quick Depth 3:
  - `Using CUDA`
  - `Using float16`
  - 기타 예외/경고 라인

예시 명령:

```powershell
Select-String "render_after_pipeline|detail boost applied" "$env:TEMP\ZSoda_AE_Runtime.log" | Select-Object -Last 30
Select-String "run_prepare_input_ok|temporal_history_reuse_duplicate_frame" "$env:TEMP\ZSoda_AE_Runtime.log" | Select-Object -Last 50
Select-String "Using CUDA|Using float16|error|Exception" "C:\Users\Yongkyu\AppData\Local\Temp\BSKL\quickdepth3_render-engine.log" | Select-Object -Last 50
```

## 8) 의사결정 게이트
- Gate-1 (즉시 완화):
  - A1 결과에서 seam score가 유의하게 감소하면, Detail Boost 기본값/적용 조건 재검토를 즉시 우선순위화.
- Gate-2 (구조 개선 착수):
  - A2/A3에서 파라미터 민감도가 높으면, 합성 방식(`ComposeTiles`) 개선을 1순위로 확정.
- Gate-3 (temporal 안전장치):
  - B2에서 부작용이 확인되면, temporal 모델에서 Detail Boost 타일링 skip 정책을 우선 반영.

## 9) Quick Depth 3 비교 시 원칙
- 바이너리 수정/무단 역공학은 하지 않는다.
- 로그/입출력 결과/설정 차이의 블랙박스 비교만 수행한다.
- 설치 파일/패키지 내 공개 텍스트/설정 파일 범위에서만 확인한다.

## 10) 결과 기록 템플릿

| Run ID | Plugin | 핵심 설정 | M1 Seam | M2 Smooth | M3 Edge | M4 Temporal | M5 ms/frame | 비고 |
|---|---|---|---:|---:|---:|---:|---:|---|
| A0 | Z-Soda | default |  |  |  |  |  |  |
| A1-off | Z-Soda | detail_boost=0 |  |  |  |  |  |  |
| A1-on | Z-Soda | detail_boost=1 |  |  |  |  |  |  |
| B2a | Z-Soda | multiview+boost off |  |  |  |  |  |  |
| B2b | Z-Soda | multiview+boost on |  |  |  |  |  |  |
| C0 | Quick Depth 3 | baseline |  |  |  |  |  |  |

