# 모델 설치 가이드

기본 우선 모델은 `Depth Anything v3` 입니다.

지원 모델 ID:
- `depth-anything-v3-small` (기본)
- `depth-anything-v3-base`
- `depth-anything-v3-large`
- `midas-dpt-large`

기본 모델 저장 루트:
- `models/` (환경 변수 `ZSODA_MODEL_ROOT`로 변경 가능)
- 기본 매니페스트: `models/models.manifest` (환경 변수 `ZSODA_MODEL_MANIFEST`로 변경 가능)
- AE 플러그인 런타임 기본 탐색: `.aex` 인접 경로의 `models/` 폴더 우선

예상 파일 경로:
- `models/depth-anything-v3/depth_anything_v3_small.onnx`
- `models/depth-anything-v3/depth_anything_v3_base.onnx`
- `models/depth-anything-v3/depth_anything_v3_large.onnx`
- `models/midas/dpt_large_384.onnx`

다운로드 스크립트:
```bash
bash tools/download_model.sh depth-anything-v3-small
```

Windows PowerShell:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\download_model.ps1 -ModelId depth-anything-v3-small
```

매니페스트 포맷 (`|` 구분 텍스트, 주석은 `#`):
```txt
# id|display_name|relative_path|download_url|preferred_default
depth-anything-v3-small|Depth Anything v3 Small|depth-anything-v3/depth_anything_v3_small.onnx|https://...|true
```

주의:
- ONNX Runtime API가 활성화된 빌드에서는 실제 추론 경로를 사용합니다.
- 모델 파일이 없거나 ORT 세션 초기화가 실패하면 안전한 폴백 경로를 사용합니다.
