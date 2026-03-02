# AE SDK `PF_Cmd` 통합 + Depth Anything 배포 실무 노트 (2026-03-02)

## 목적
- 현재 스텁 구조를 AE SDK 실제 엔트리/콜백 경로로 연결하기 위한 구현 포인트를 고정한다.
- Depth Anything 모델 설치/선택/실행/폴백 흐름을 현재 코드 기준으로 정리한다.

## 근거 소스
- 내부 코드
  - `plugin/ae/AePluginEntry.cpp` (엔트리 스텁, 정적 엔진/파이프라인/라우터)
  - `plugin/ae/AeCommandRouter.cpp` (명령 분기, 파라미터 적용)
  - `plugin/ae/AeParams.h`, `plugin/ae/AeParams.cpp` (파라미터 스키마/클램프)
  - `plugin/core/RenderPipeline.cpp` (캐시 우선, 실패 시 타일 폴백, Safe output)
  - `plugin/inference/ManagedInferenceEngine.cpp` (모델 선택, 파일 존재 확인, 현재 더미 추론 경로)
  - `plugin/inference/ModelCatalog.cpp`, `tools/download_model.sh` (모델 ID/URL/경로)
- 외부 공식 문서
  - Adobe Entry Point: https://ae-plugins.docsforadobe.dev/effect-basics/entry-point/
  - Adobe Command Selectors: https://ae-plugins.docsforadobe.dev/effect-basics/command-selectors/
  - Adobe Parameters: https://ae-plugins.docsforadobe.dev/effect-basics/parameters/
  - Adobe SmartFX: https://ae-plugins.docsforadobe.dev/smartfx/smartfx/
  - ONNX Runtime C/C++ 시작: https://onnxruntime.ai/docs/get-started/with-c.html
  - ONNX Runtime EP 개요: https://onnxruntime.ai/docs/execution-providers

## 1) 현재 코드 기준 호출 흐름 (확정)
1. 엔트리 스텁이 `command_id`를 내부 `AeCommand`로 매핑 (`plugin/ae/AePluginEntry.cpp:14`).
2. 렌더 시 `RenderRequest` + `AeParamValues`를 `RenderParams`로 변환 (`plugin/ae/AeCommandRouter.cpp:31`, `plugin/ae/AeParams.cpp:11`).
3. `RenderPipeline::Render()`에서:
   - 모델 선택 (`plugin/core/RenderPipeline.cpp:44`)
   - 캐시 조회 (`plugin/core/RenderPipeline.cpp:49`)
   - 추론 실패 시 타일 폴백 (`plugin/core/RenderPipeline.cpp:62`)
   - 최종 실패 시 안전 출력 (`plugin/core/RenderPipeline.cpp:181`)
4. 현재 `ManagedInferenceEngine::Run()`은 ORT 실추론이 아니라 더미 경로 + 모델별 bias (`plugin/inference/ManagedInferenceEngine.cpp:56`, `plugin/inference/ManagedInferenceEngine.cpp:64`).

## 2) AE SDK `PF_Cmd_*` 매핑 제안 (현재 코드 -> 목표 콜백)
| AE SDK selector | 현재 코드 앵커 | 구현 제안 |
|---|---|---|
| `PF_Cmd_ABOUT` | `AeCommand::kAbout` (`plugin/ae/AeCommandRouter.cpp:16`) | `out_data->return_msg` 채우기만 담당. |
| `PF_Cmd_GLOBAL_SETUP` | `AeCommand::kGlobalSetup` (`plugin/ae/AeCommandRouter.cpp:19`) | 모델 메뉴 갱신 + `out_data->my_version`, `out_flags`, `out_flags2` 설정. SmartFX 도입 시 `SUPPORTS_SMART_RENDER` 플래그도 여기서 설정. |
| `PF_Cmd_PARAM_SETUP` | 현재는 메뉴 갱신만 수행 (`plugin/ae/AeCommandRouter.cpp:20`) | `AeParamId` 기준으로 실제 `PF_ADD_*` 등록 추가. `out_data->num_params`를 실제 개수와 일치시킴. |
| `PF_Cmd_SEQUENCE_SETUP` | 현재 없음 | 인스턴스별 시퀀스 데이터 핸들에 라우터 상태/캐시 정책 저장. 현재 정적 싱글톤(`plugin/ae/AePluginEntry.cpp:29`)은 다중 인스턴스 분리에 취약. |
| `PF_Cmd_SEQUENCE_RESETUP`/`SETDOWN`/`FLATTEN` | 현재 없음 | 시퀀스 데이터 복원/해제/평탄화 처리. 프로젝트 저장/복제 시 상태 일관성 확보. |
| `PF_Cmd_USER_CHANGED_PARAM` | 현재 없음 | 변경 파라미터만 읽어 `UpdateParams()` 호출. `model_id`, `tile_size`, `overlap` 변경 시 캐시 purge 후보. |
| `PF_Cmd_UPDATE_PARAMS_UI` | 현재 없음 | `output_mode != slicing`이면 `min/max/softness` UI 비활성화. 값 변경은 금지(UI 상태만 변경). |
| `PF_Cmd_RENDER` | 스텁 주석만 존재 (`plugin/ae/AePluginEntry.cpp:51`) | `PF_EffectWorld` -> 내부 float 프레임 변환 후 `router.Handle(kRender)` 호출, 결과를 output world에 기록. |
| `PF_Cmd_SMART_PRE_RENDER`/`SMART_RENDER` | 현재 없음 | SmartFX로 갈 경우 비-레이어 파라미터는 체크아웃 기반으로 읽고, pre-render에서 필요한 입력을 선언. `RENDER` 경로와 병행 지원 권장. |

## 3) 파라미터 흐름 매핑 (구체)
| 내부 ID | AE 파라미터 타입 제안 | `AeParamValues` 필드 | `RenderPipeline` 사용 지점 |
|---|---|---|---|
| `kModel` | Popup | `model_id` | 모델 선택 + 캐시 키 model hash (`plugin/core/RenderPipeline.cpp:44`, `plugin/core/RenderPipeline.cpp:117`) |
| `kQuality` | Popup/Slider(1~3) | `quality` | `RunInference` 요청 품질 (`plugin/core/RenderPipeline.cpp:122`) |
| `kOutputMode` | Popup(Depth/Slicing) | `output_mode` | `BuildOutput` 분기 (`plugin/core/RenderPipeline.cpp:174`) |
| `kInvert` | Checkbox | `invert` | `NormalizeDepth` (`plugin/core/RenderPipeline.cpp:69`, `plugin/core/RenderPipeline.cpp:85`) |
| `kMinDepth` | Float Slider(0~1) | `min_depth` | Slicing 구간 시작 (`plugin/core/RenderPipeline.cpp:176`) |
| `kMaxDepth` | Float Slider(0~1) | `max_depth` | Slicing 구간 끝 (`plugin/core/RenderPipeline.cpp:176`) |
| `kSoftness` | Float Slider(0~1) | `softness` | Slicing feather (`plugin/core/RenderPipeline.cpp:176`) |
| `kCacheEnable` | Checkbox | `cache_enabled` | 캐시 hit/insert on/off (`plugin/core/RenderPipeline.cpp:49`, `plugin/core/RenderPipeline.cpp:89`) |
| `kTileSize` | Slider(Int) | `tile_size` | 타일 폴백 크기 (`plugin/core/RenderPipeline.cpp:63`, `plugin/core/RenderPipeline.cpp:152`) |
| `kOverlap` | Slider(Int) | `overlap` | 타일 중첩 (`plugin/core/RenderPipeline.cpp:63`, `plugin/core/RenderPipeline.cpp:152`) |

추가 메모:
- `ToRenderParams()`에서 클램프/보정이 이미 구현됨 (`plugin/ae/AeParams.cpp:14`~`27`).
- `frame_hash`는 현재 외부 입력값을 그대로 사용 (`plugin/ae/AeCommandRouter.cpp:35`). AE 연동 시 comp/layer/time 기반 해시 규칙을 별도로 고정해야 캐시 효율이 안정적임.

## 4) 픽셀 경계(8/16/32 bpc) 연동 메모
- 현재 렌더 스텁은 `Gray32F` 입력만 가정 (`plugin/ae/AePluginEntry.cpp:80`~`90`).
- 실제 `PF_Cmd_RENDER` 연결 시 권장:
  1. 입력 world를 포맷별(8/16/32)로 읽어 내부 `float`로 정규화
  2. 내부 연산은 기존대로 `Gray32F` 유지
  3. 출력 depth/slice를 output world 포맷으로 역변환
- Adobe 문서상 `PF_EffectWorld`는 rowbytes/pixel depth를 명시적으로 다뤄야 하므로, 포맷별 변환 루틴을 별도 파일로 분리하는 편이 안전함.

## 5) Depth Anything 배포 워크플로 (현재 코드 기준)
1. 모델 카탈로그 ID/URL/상대경로는 `ModelCatalog`에 하드코딩 (`plugin/inference/ModelCatalog.cpp:7`~`35`).
2. 설치 루트는 `ZSODA_MODEL_ROOT` 우선, 없으면 `models/` (`plugin/inference/EngineFactory.cpp:11`~`15`).
3. 다운로드는 `tools/download_model.sh`에서 동일 ID 집합을 사용 (`tools/download_model.sh:10`~`32`).
4. 런타임 모델 선택은 `SelectModel()`에서 수행, 파일 미존재 시에도 폴백 경로로 실행 (`plugin/inference/ManagedInferenceEngine.cpp:93`~`113`, `76`~`80`).
5. 파이프라인은 모델별 캐시 분리를 이미 지원 (`plugin/core/RenderPipeline.cpp:117`, `tests/test_render_pipeline.cpp:25`~`47`).

## 6) ORT 연결 시 최소 수정 포인트
- `ManagedInferenceEngine::SelectModelLocked()`
  - 모델 파일 존재 확인 후, 모델별 `Ort::Session` 생성/재사용 캐시 추가.
- `ManagedInferenceEngine::Run()`
  - 현재 `fallback_engine_.Run()` 호출부를 ORT 추론 호출로 대체.
  - 실패 시 현재와 동일하게 에러 문자열 반환 + 안전 폴백 유지.
- `CreateDefaultEngine()`
  - 초기 `Initialize()` 실패 시 `nullptr` 반환 대신 명시 오류 로깅/상태 전달 경로 검토.
- `ModelCatalog`와 `download_model.sh`
  - 동일 ID/URL 중복 정의가 있어 drift 위험 존재. 단일 manifest 기반으로 통합 필요.

## 7) 바로 착수 가능한 구현 순서 (AE Host 관점)
1. 실제 AE 엔트리 함수에 `PF_Cmd` 스위치 추가 (`ABOUT/GLOBAL_SETUP/PARAM_SETUP/RENDER` 우선).
2. `PARAM_SETUP`에서 `AeParamId` 10개를 `PF_ADD_*`로 등록.
3. 렌더 시 `params[]` -> `AeParamValues` 파싱 함수 추가 후 `UpdateParams`/`params_override`에 연결.
4. `PF_Cmd_RENDER`에서 world 변환 함수(8/16/32)를 통해 `RenderPipeline` 호출.
5. `USER_CHANGED_PARAM` + `UPDATE_PARAMS_UI`를 붙여 slicing UI/캐시 정책 제어.
6. `SEQUENCE_*` 단계로 정적 싱글톤 의존 제거(인스턴스 안전성 확보).

