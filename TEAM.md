# 에이전트 팀 운영안

초기 버전 완성을 위해 아래 6개 역할로 병렬 작업합니다.

## 1) Lead / Integrator (리드)
- 책임 파일: 전체(통합 책임)
- 역할:
  - 인터페이스 고정/변경 승인 (`AeParams`, `RenderPipeline`, `InferenceEngine`)
  - 병렬 작업 머지 순서 관리
  - 코드 리뷰, PR 작성/정리, 릴리즈 노트 작성
  - `PROGRESS.md` 최종 업데이트 승인

## 2) AE Host Agent
- 책임 파일: `plugin/ae/*`
- 역할:
  - AE 파라미터/커맨드 핸들러 연결
  - `PF_Cmd_*` 대응 및 SDK 연동 준비
  - 모델 선택 UI(`model_id`) 노출

## 3) Inference Agent
- 책임 파일: `plugin/inference/*`, `models/*`, `tools/download_model.sh`
- 역할:
  - 모델 카탈로그/세션 재사용/모델 전환
  - ONNX Runtime CPU 경로 우선 구현
  - OS별 EP(CUDA/DirectML/CoreML) 분기 설계

## 4) Core Pipeline Agent
- 책임 파일: `plugin/core/*`
- 역할:
  - 캐시/타일링/폴백/OOM 처리
  - Depth Map/Slicing 출력 안정화
  - 8/16/32 bpc 경계 변환 정합성 보장

## 5) QA/Perf Agent
- 책임 파일: `tests/*`, `tools/*(검증 도구)`
- 역할:
  - 회귀/단위/장시간 안정성 테스트
  - 성능 지표(ms/frame, cache hit rate, VRAM) 수집
  - PR 성능 영향 비교표 작성

## 6) Research Agent (신규)
- 책임 파일: `docs/research/*`
- 역할:
  - Adobe AE SDK 공식 문서 추적
  - Depth Anything/유사 프로젝트 코드 레퍼런스 분석
  - 설계 의사결정 근거(링크+요약) 제공

## 운영 규칙
- 작업 단위 완료 시 `PROGRESS.md`를 즉시 갱신
- 커밋 메시지: `module: summary`
- GitHub 업데이트 주기:
  - 최소 1일 1회 push
  - 또는 의미 있는 작업 단위(P1~P5 하위 완료)마다 push
- PR은 리드가 최종 소유(리뷰/체크리스트/머지)
