# 2026-03-06 Multi-View/품질 개선 분석 및 실행안

## 1) 이번 문서의 목적
- 타일링 seam 이슈는 해결된 상태에서, 남아있는 **품질 격차(QD3 대비 뭉개짐/평면화/일관성 저하)**의 원인을 구조적으로 정리한다.
- DA3 공식 구현과 ComfyUI Multi-View 노드의 동작을 기준으로, Z-Soda에 적용할 **실행 가능한 단계별 개선안**을 정의한다.

## 2) 확인된 사실 (근거 포함)

### A. DA3 공식 입력 리사이즈는 `upper_bound_resize + patch(14) 정렬`
- DA3 공식 `InputProcessor`는 `process_res_method="upper_bound_resize"`일 때 장축 기준으로 비율 유지 리사이즈를 먼저 수행한다.
- 이후 폭/높이를 `PATCH_SIZE=14` 배수로 맞춘다. `*resize` 계열은 nearest multiple 정렬 방식이다.
- 즉, 정사각 고정 리사이즈가 아니라 **종횡비 유지 + 14 배수 정렬**이 기본이다.

참고:
- https://raw.githubusercontent.com/ByteDance-Seed/Depth-Anything-3/main/depth_anything_3/utils/io/input_processor.py
- https://github.com/ByteDance-Seed/Depth-Anything-3/blob/main/docs/python_api.md

예시(공식 코드 로직 기준):
- 입력 1920x1080, process_res=518
- upper-bound 스케일 후 518x291
- patch 14 정렬 후 518x294

### B. DA3 공식 `ref_view_strategy`는 기본 `saddle_balanced`, 비디오에는 `middle` 권장
- 공식 문서에서 reference view 전략은 `saddle_balanced`가 default.
- 다만 비디오 입력은 `middle`이 안정적일 수 있다고 명시되어 있음.

참고:
- https://raw.githubusercontent.com/ByteDance-Seed/Depth-Anything-3/main/docs/funcs/ref_view_strategy.md

### C. ComfyUI DA3 Multi-View 노드는 “진짜 N-view 배치 추론”을 수행
- 입력 배치 `[N,H,W,C]`를 `[1,N,C,H,W]`로 만들어 한 번의 forward로 처리한다.
- 모델 capability를 점검하고, cross-view attention 지원 여부를 경고한다.
- depth normalization도 “view 전체를 함께” 처리해 프레임 간 일관성을 높인다.

참고:
- https://raw.githubusercontent.com/PozzettiAndrea/ComfyUI-DepthAnythingV3/main/nodes/nodes_multiview.py
- https://github.com/PozzettiAndrea/ComfyUI-DepthAnythingV3

### D. ComfyUI의 장영상 안정화는 Streaming(Chunk + Overlap + Sim(3) 정렬 + 블렌딩)까지 사용
- chunk 단위 멀티뷰 추론 후, overlap 구간 point cloud 기반 Sim(3) 정렬.
- 마지막에 overlap 선형 블렌딩으로 chunk 경계 flicker/scale jump를 줄인다.

참고:
- https://raw.githubusercontent.com/PozzettiAndrea/ComfyUI-DepthAnythingV3/main/nodes/streaming/pipeline.py
- https://raw.githubusercontent.com/PozzettiAndrea/ComfyUI-DepthAnythingV3/main/nodes/streaming/node.py

## 3) 현재 Z-Soda 상태 요약 (코드 기준)

### A. 리사이즈 경로
- Z-Soda는 DA3 프로파일에서 `use_upper_bound_dynamic_aspect=true`를 사용하며, 동적 텐서 크기를 계산한다.
- `ZSODA_DA3_PROCESS_RES` 기본은 품질 tier 연동(quality 1/2/3 -> 512/640/768)로 동작한다.
- 선형->sRGB 변환 기본값은 `true`.

관련 코드:
- `plugin/inference/OnnxRuntimeBackend.cpp` (ResolveDa3ProcessResolution, ComputeUpperBoundAspectTensorSize, PrepareInputForModel)

### B. Multi-View 경로
- 현재 런타임은 프레임 히스토리를 쌓아 `[1,T,C,H,W]` 입력을 구성하는 temporal history 방식.
- 이 방식은 “여러 실제 뷰를 동시 처리하는 배치 추론”과 다르며, AE 단일 프레임 호출 기반에서 history 재구성 형태로 동작한다.

관련 코드:
- `plugin/inference/OnnxRuntimeBackend.cpp` (BuildTemporalInput, frame_hash 중복 처리, ref strategy 선택)

### C. 타일링 회귀 가드
- temporal sequence 모델에서 detail-boost 타일 재진입/일반 tiled fallback을 스킵하는 가드가 들어가 있음.
- 이전 seam 회귀의 직접 원인 경로는 차단된 상태.

관련 코드:
- `plugin/core/RenderPipeline.cpp` (`IsTemporalSequenceModelId`, detail boost/tiled fallback skip)

## 4) QD3 대비 품질 격차의 핵심 가설

1. **멀티뷰 구조 차이**
- QD3/Comfy 노드의 이점은 “실제 프레임 묶음 동시 추론 + 교차 주의”인데,
- Z-Soda는 현재 history 재구성 temporal 입력으로 유사 동작을 흉내 내는 구조라서, 프레임 간 안정성과 디테일 보존에서 불리할 수 있음.

2. **정규화/표시 단계 차이**
- Comfy/QD 계열은 V2-style(역깊이 기반, sky/content 분리, 퍼센타일 대비 강화) 경향이 강함.
- Z-Soda는 guided percentile mapping 중심이라, 장면에 따라 배경이 거칠거나 엣지가 뭉개져 보일 수 있음.

3. **출력 선택 전략 차이**
- 멀티맵 출력에서 어떤 map을 뽑는지(ref view)와 스케일 정합이 미세하게 다르면, 체감 품질이 크게 달라짐.

4. **(추론) QD3의 “Quality Boost”는 해상도/경로 제어 가능성**
- QD3에서 256~2048 px 품질 프리셋과 Time Consistency/Quality Boost 문자열이 확인됨.
- 모델 교체보다는 내부 process resolution, 멀티뷰/스트리밍 경로, 후처리 강도 조합일 가능성이 높음.
- 이 항목은 바이너리 문자열 기반 추론이며, 소스 미공개라 확정은 아님.

## 5) 실행 계획 (우선순위)

## Phase 1. 관측 가능한 품질 차이 축소 (단기)
- P1-1. DA3 입력 리사이즈 검증 로그 강화
  - 프레임마다 `src -> tensor`, `process_res`, `patch align`, `resize_mode`를 trace에 남겨 실제 경로를 고정 검증.
- P1-2. 표시 정규화 모드 확장
  - `guided` 외에 `v2_style_like`(inverse-depth + sky/content 분리 + 퍼센타일) 실험 모드 추가.
- P1-3. near/far 표시 일관화
  - AE 기본값을 `near=bright`에 고정하고, UI/invert 의미를 명확히 분리(연산용 invert vs 표시 invert).

성공 기준:
- 동일 샷 A/B에서 배경 그라데이션 노이즈/거칠기 감소.
- 인물/오브젝트 엣지 blur 감소.

## Phase 2. Multi-View 동작 정합 (중기)
- P2-1. “진짜 N-view 동시 입력” 모드 추가 검토
  - AE 프레임 요청 패턴에 맞는 뷰 윈도우 버퍼 설계 필요.
  - 가능한 경우 `[1,N,C,H,W]`를 실제 인접 프레임으로 구성.
- P2-2. ref_view_strategy 정책화
  - 기본 `middle` 유지, shot 특성별 `first/latest/saddle_balanced`를 스위치 가능하게.
- P2-3. 중복 frame_hash/scene-cut 정책 정교화
  - history 재사용 오탐을 줄이는 조건을 추가하고, tile path와 분리 유지.

성공 기준:
- 동일 영상에서 프레임 간 flicker/펌핑 감소.
- 컷 전환 후 안정화 수렴 시간 단축.

## Phase 3. Streaming형 안정화 (중장기)
- P3-1. chunk+overlap 파이프라인 프로토타입
- P3-2. overlap 구간 정렬/블렌딩(간소화 버전) 도입
- P3-3. VRAM/지연 예산에 따른 자동 tier 선택

성공 기준:
- 긴 시퀀스에서 depth scale jump 감소.
- VRAM OOM 없이 일정한 품질 유지.

## 6) 바로 진행 가능한 실험 세트
- E1. 동일 컷에서 quality 1/2/3 + process_res(512/640/768) + mapping_mode 조합 렌더.
- E2. `ZSODA_DA3_MULTIVIEW_REF=middle|latest` 비교.
- E3. 장면별(실내 저광/고대비/배경 디테일 많은 샷)에서 edge 보존 지표와 flicker 지표 비교.

산출물:
- 샘플 프레임 A/B 이미지
- 프레임간 차분(절대차 평균)
- 파이프라인 로그 스냅샷

## 7) 결론
- 현재 핵심은 “타일링”이 아니라 “멀티뷰 구조/정규화/출력 선택 전략의 정합”이다.
- QD3 체감 품질 차이는 모델 자체보다, 입력 스케일링 + 멀티뷰 처리 방식 + 표시 정규화 조합에서 발생했을 가능성이 높다.
- 우선 Phase 1로 빠르게 체감 품질을 올리고, 이후 Phase 2/3에서 멀티뷰 구조를 근본 개선하는 순서가 안전하다.
