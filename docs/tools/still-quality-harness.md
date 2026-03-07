# Still Quality Harness

`zsoda_still_quality_harness`와 비교 스크립트는 AE를 띄우지 않고도 Z-Soda의 still 품질을 반복 검증하기 위한 오프라인 루프입니다.

## 목적

- AE host 경계와 무관하게 core depth 파이프라인 품질만 본다.
- 같은 입력 이미지에 대해 `QD3`, `official DA3`, `Z-Soda raw`, `Z-Soda pipeline`을 한 번에 비교한다.
- 중간 산출물과 로그를 구조적으로 저장해 원인 분석 속도를 높인다.

## 1. 빌드

```powershell
cmake -S C:\Users\Yongkyu\code\Z-Soda -B C:\Users\Yongkyu\code\Z-Soda\build-win -DZSODA_BUILD_TOOLS=ON
cmake --build C:\Users\Yongkyu\code\Z-Soda\build-win --config RelWithDebInfo --target zsoda_still_quality_harness
```

산출물 기본 위치:

```text
C:\Users\Yongkyu\code\Z-Soda\build-win\tools\RelWithDebInfo\zsoda_still_quality_harness.exe
```

## 2. Z-Soda 단독 하네스 실행

```powershell
C:\Users\Yongkyu\code\Z-Soda\build-win\tools\RelWithDebInfo\zsoda_still_quality_harness.exe `
  --input C:\path\frame.png `
  --output-dir C:\path\artifacts\still_run `
  --model-root C:\Users\Yongkyu\code\Z-Soda\models `
  --model-id depth-anything-v3-large-multiview `
  --backend auto `
  --resize-mode upper_bound_letterbox `
  --quality 1 `
  --mapping-mode v2-style `
  --raw-visualization minmax
```

주요 산출물:

- `input_source.png`
- `zsoda_raw_depth.png`
- `zsoda_raw_depth.npy`
- `zsoda_pipeline_depth.png`
- `zsoda_pipeline_depth.npy`
- `zsoda_runtime_trace_tail.txt`
- `resolved_config.json`

## 3. Official DA3 wrapper

`tools/official_da3_still.py`는 로컬 `Depth-Anything-3` repo와 Python 의존성이 준비된 환경에서 official DA3 depth를 생성합니다.

필수 Python 패키지:

- `torch`
- `numpy`
- `Pillow`
- `huggingface_hub`
- `omegaconf`
- `addict`
- `einops`
- `imageio`
- `Depth-Anything-3` repo import 가능 상태
- 래퍼는 still 생성에 불필요한 `export`/`pose_align` 의존성을 내부에서 우회하므로, 전체 official extras를 모두 설치하지 않아도 동작하도록 구성했다.

예시:

```powershell
python C:\Users\Yongkyu\code\Z-Soda\tools\official_da3_still.py `
  --input C:\path\frame.png `
  --output-dir C:\path\artifacts\official_da3 `
  --repo-root C:\Users\Yongkyu\code\Z-Soda\.tmp_external_research\Depth-Anything-3 `
  --model-repo depth-anything/DA3-LARGE-1.1 `
  --process-res 504 `
  --process-res-method upper_bound_resize `
  --device auto
```

주요 산출물:

- `official_da3_depth.png`
- `official_da3_depth.npy`
- `official_da3_metadata.json`

## 4. 전체 비교 번들 생성

```powershell
python C:\Users\Yongkyu\code\Z-Soda\tools\still_quality_compare.py `
  --input C:\path\frame.png `
  --qd3-image C:\path\qd3_depth.png `
  --output-dir C:\path\artifacts\compare_bundle `
  --model-root C:\Users\Yongkyu\code\Z-Soda\models `
  --model-id depth-anything-v3-large-multiview `
  --quality 1 `
  --backend auto `
  --resize-mode upper_bound_letterbox `
  --mapping-mode v2-style
```

생성 결과:

- `comparison_report.html`
- `comparison_manifest.json`
- `logs/*.txt`
- `zsoda/` 하네스 산출물
- `official_da3/` official wrapper 산출물
- `references/qd3_reference.png`

## 5. 권장 루프

1. 같은 입력 프레임과 같은 QD3 기준 이미지를 고정한다.
2. `still_quality_compare.py`로 비교 번들을 반복 생성한다.
3. `resolved_config.json`과 `official_da3_metadata.json`을 함께 보면서 preprocess / mapping / backend 차이를 확인한다.
4. still 품질 격차가 줄어든 뒤에만 시퀀스 플리커 문제로 넘어간다.

## 6. 주의사항

- official DA3 wrapper는 의존성이나 체크포인트가 없으면 실패할 수 있다. 이 경우 `--official-image`로 이미 생성된 reference 이미지를 직접 넣어도 된다.
- Z-Soda 하네스는 `%TEMP%\\ZSoda_AE_Runtime.log`의 tail만 수집한다. 직전 런의 로그가 섞일 수 있으므로, 필요하면 테스트 전 로그를 정리한다.
- 비교는 still 기준이다. 시퀀스 consistency나 streaming alignment는 별도 하네스로 확장해야 한다.

## 8. Desktop GUI

QD3 결과를 직접 업로드하고 official DA3 / Z-Soda를 한 번에 생성해 비교하려면 Tkinter GUI를 쓰는 편이 빠르다.

필수 Python 패키지:

- `Pillow`
- `tkinter` 사용 가능한 Python

실행:

```powershell
python C:\Users\Yongkyu\code\Z-Soda\tools\still_compare_gui.py
```

기존 비교 번들을 바로 열고 시작:

```powershell
python C:\Users\Yongkyu\code\Z-Soda\tools\still_compare_gui.py `
  --bundle-dir C:\Users\Yongkyu\code\Z-Soda\artifacts\still_compare_official_default
```

GUI에서 할 수 있는 작업:

- source / QD3 / official 이미지 경로 선택
- official DA3 auto-generate 또는 수동 이미지 비교 전환
- Z-Soda quality / resize / mapping / guided / temporal / edge 파라미터 조정
- `Run Compare`로 새 번들 생성
- `Load Bundle`로 기존 `comparison_manifest.json` 번들 재열기
- preview pane에서 source / QD3 / official / Z-Soda raw / Z-Soda pipeline 즉시 확인
- `Open Report`, `Open Output`으로 HTML 리포트와 산출물 폴더 열기

## 9. Video Input

`tools/still_quality_compare.py`와 GUI는 영상 파일을 직접 받을 수 있다. 다만 현재 동작은 "영상 전체 비교"가 아니라, `ffmpeg`로 지정 시점의 한 프레임을 뽑아 still 비교 하네스로 넘기는 방식이다.

CLI 예시:

```powershell
python C:\Users\Yongkyu\code\Z-Soda\tools\still_quality_compare.py `
  --input C:\path\clip.mp4 `
  --video-time-seconds 1.25 `
  --output-dir C:\path\artifacts\compare_bundle `
  --model-root C:\Users\Yongkyu\code\Z-Soda\models `
  --model-id depth-anything-v3-large-multiview `
  --backend auto `
  --resize-mode upper_bound_letterbox `
  --quality 1 `
  --mapping-mode raw
```

GUI에서는 source에 영상 파일을 넣고 `Video Time (s)` 값만 지정하면 된다. 비교 번들에는 원본 영상 경로, 추출된 프레임 경로, `ffmpeg` 추출 로그가 함께 남는다.
