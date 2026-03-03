# TheAnimeScripter 참조 분석 (2026-03-03)

목적: `NevermindNilas/TheAnimeScripter` 내부 소스를 참조해 Z-Soda 안정성/운영 설계에 바로 적용 가능한 패턴을 정리한다.

## 확인한 핵심 포인트
- AE 연동 방식이 `.aex` 내부 렌더 경로 직접 실행보다 **외부 프로세스 + 소켓/큐 통신**에 가깝다.
  - 근거: `src/utils/aeComms.py`, `src/utils/argumentsChecker.py (--ae)`
- Depth 런타임은 provider 가용성 기반으로 **GPU 우선 -> CPU fallback**을 강제한다.
  - 근거: `src/depth/depth.py` (`DepthDirectMLV2`, `DepthOpenVinoV2` 등)
- 모델 다운로드/경로 결정/상태 업데이트를 중앙 함수로 모아 **경로 누락/부분 다운로드**를 방어한다.
  - 근거: `src/utils/downloadModels.py`

## Z-Soda에 반영할 구조적 방향
1. AE 프로세스 내 하드 크래시를 줄이기 위해, 추론 경로를 장기적으로 `out-of-process worker` 옵션까지 확장한다.
2. ORT 로딩은 호스트 DLL 탐색에 의존하지 않고 **플러그인 인접 경로(절대경로) 우선**으로 고정한다.
3. backend/provider 실패 시 즉시 안전 출력으로 복귀하고, 실패 원인을 짧은 진단 로그로 남긴다.

## 복사 금지/주의 항목
- `TheAnimeScripter`는 배치/옵션 조합이 매우 넓어 그대로 복제하면 복잡도만 증가한다.
- 일부 분기에는 버그 가능 코드가 보인다(예: 모델 매핑 누락 case). 패턴만 참조하고 구현은 Z-Soda 기준으로 재설계한다.

## 이번 라운드 적용 여부
- [x] ORT 동적 로드 기본 경로를 side-by-side(플러그인 인접 DLL) 우선으로 강화
- [x] AE entrypoint/stub에 예외 방어층(SEH + C++ 예외 로그) 추가
- [ ] out-of-process worker 모드는 별도 단계(P6 후보)로 설계
