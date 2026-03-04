# 2026-03-04 Depth Quality Improvement Review (DA3/DS2 Reference)

## 목적
- 리서치 에이전트가 정리한 품질 개선안을 Z-Soda 코드베이스 기준으로 검토하고,
  실행 가능한 우선순위/수용 기준으로 정리한다.
- 대상 증상:
  - 단일 프레임 품질 저하(엣지/경계 분리 약함)
  - 영상 시퀀스 플리커(프레임 간 깊이 분포 흔들림)

## 종합 결론
- 제안서의 핵심 진단은 타당하다.
- 특히 아래 3개는 "근본 원인"으로 우선 처리하는 것이 맞다.
  1. 입력을 Gray로 축약해 RGB 단서를 잃는 구조
  2. 518x518 강제 스트레치(종횡비 비보존)
  3. 프레임별 정규화 중심 매핑(및 중복 정규화 가능성)

## 코드 기준 확인 포인트 (main 기준)
- 입력 Gray 축약 경로
  - `plugin/ae/AeHostAdapter.cpp`: `BuildHostBufferRenderDispatch()`에서 `ConvertHostToGray32F()` 호출
  - `plugin/core/PixelConversion.h`: `ConvertHostToGray32F()`가 RGBA -> luma 1채널 생성
  - `plugin/inference/OnnxRuntimeBackend.cpp`: 채널 샘플링 시 단일채널 입력이 사실상 3채널 복제로 사용됨
- 종횡비 비보존 리사이즈
  - `plugin/inference/OnnxRuntimeBackend.cpp`: `PrepareInputForModel()`에서 X/Y 독립 스케일로 518x518 매핑
- 정규화 구조
  - `plugin/inference/OnnxRuntimeBackend.cpp`: 모델 출력 min/max 기반 정규화
  - `plugin/core/RenderPipeline.cpp`: 후단 `NormalizeDepth()` 추가 적용

## 우선순위 개선안

### P0. RGB 입력 경로 강제 (필수)
- 목표: 추론 입력을 Gray가 아닌 RGB(또는 RGBA에서 RGB 추출)로 전달.
- 원칙:
  - Host buffer -> RGB float 프레임으로 변환
  - Alpha는 입력 특성에 따라 무시 또는 언프리멀트 옵션 제공
- 기대 효과:
  - 경계/재질/조명 단서 회복
  - 단일 프레임 품질 즉시 개선

### P1. 종횡비 보존 리사이즈 + 패딩/크롭 (필수)
- 목표: 정사각형 강제 스트레치 제거.
- 구현 방향:
  - `process_res_method` 개념 도입 (`upper_bound`/`lower_bound`)
  - letterbox padding 또는 center crop 정책 분리
- 기대 효과:
  - 형태 왜곡 감소
  - 프레임 간 예측 불안정 완화

### P2. 매핑 모드 분리 + 정규화 단일화 (플리커 핵심)
- 목표: 프레임별 min/max 정규화를 기본값에서 분리.
- 구현 방향:
  - `Raw / Normalize / Guided` 매핑 모드 도입
  - 백엔드 출력은 가능한 raw 유지, 표시/출력 단계에서만 매핑
  - 중복 정규화 금지(정규화 책임 1곳으로 고정)
- 기대 효과:
  - 시퀀스 플리커 대폭 감소

### P3. 시간 안정화 옵션 (Subpixel/Temporal)
- 목표: 비디오 플리커 억제 옵션 제공.
- 구현 방향:
  - 1차: EMA (`D_t = lerp(D_{t-1}, D_raw, alpha)`)
  - 2차: RGB edge-aware temporal weighting
  - 가능 시 confidence 기반 가중(낮은 conf 영역에 강한 안정화)

### P4. 엣지 가이드 업샘플/강화
- 목표: 저해상도 추론의 경계 손실 보완.
- 구현 방향:
  - Joint bilateral 또는 guided filter 업샘플
  - edge-aware sharpening(옵션)

## 실행 체크리스트 (에이전트 전달용)
1. 입력 변환:
   - Gray 변환 경로 제거
   - RGB 변환 API 신설/사용
   - 관련 단위테스트 추가
2. 리사이즈:
   - 종횡비 보존 정책 추가
   - padding/crop 선택 파라미터화
3. 매핑:
   - Raw/Normalize/Guided 모드 추가
   - 정규화 단일 책임화
4. 시간 안정화:
   - EMA 옵션 추가(기본 Off)
   - scene cut/seek 시 상태 초기화 규칙 정의
5. 측정:
   - 단일 프레임 품질: 엣지 주변 depth gradient 일관성
   - 시퀀스 안정성: 픽셀별 temporal variance, 프레임간 depth histogram drift

## 리스크 / 주의사항
- 색관리(Linear/sRGB/HDR) 경로를 단순 clamp만으로 처리하면 HDR 소스 계조 손실 가능.
- temporal smoothing은 엣지 블러 트레이드오프가 있으므로 기본값 보수적으로 설정 필요.
- Guided 모드는 UX 복잡도를 높이므로 P2에서 최소 기능부터 도입 권장.

## 참조
- ByteDance-Seed / Depth-Anything-3 API/Repo
- Blace Plugins Depth Scanner 2 문서
- Z-Soda main 코드:
  - `plugin/core/PixelConversion.h`
  - `plugin/ae/AeHostAdapter.cpp`
  - `plugin/inference/OnnxRuntimeBackend.cpp`
  - `plugin/core/RenderPipeline.cpp`
