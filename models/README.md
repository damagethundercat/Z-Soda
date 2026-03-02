# 모델 설치 가이드

기본 우선 모델은 `Depth Anything v3` 입니다.

지원 모델 ID:
- `depth-anything-v3-small` (기본)
- `depth-anything-v3-base`
- `depth-anything-v3-large`
- `midas-dpt-large`

기본 모델 저장 루트:
- `models/` (환경 변수 `ZSODA_MODEL_ROOT`로 변경 가능)

예상 파일 경로:
- `models/depth-anything-v3/depth_anything_v3_small.onnx`
- `models/depth-anything-v3/depth_anything_v3_base.onnx`
- `models/depth-anything-v3/depth_anything_v3_large.onnx`
- `models/midas/dpt_large_384.onnx`

다운로드 스크립트:
```bash
bash tools/download_model.sh depth-anything-v3-small
```

주의:
- 현재 스캐폴드에서는 ONNX Runtime 실제 추론 연결 전 단계입니다.
- 모델 파일이 없으면 안전하게 폴백 경로를 사용합니다.
