# Local Windows Agent Handoff Guide

이 문서는 로컬(Windows) 에이전트가 `.aex` 빌드와 AE 스모크 테스트를 이어서 수행하기 위한 실행 지침이다.

## 1) 작업 루트

- 저장소 루트(예시): `C:\Users\Yongkyu\code\Z-Soda`
- 필수 확인:
  - `tools\build_aex.ps1`
  - `tools\package_plugin.ps1`
  - `docs\build\AE_SMOKE_TEST.md`

## 2) 시작 전 동기화

CMD:
```cmd
cd /d C:\Users\Yongkyu\code\Z-Soda
git fetch origin
git checkout main
git pull --ff-only origin main
git rev-parse --short HEAD
```

## 3) 환경 변수 설정 (CMD)

```cmd
set "AE_SDK_ROOT=C:\SDKs\AdobeAfterEffectsSDK\AfterEffectsSDK_25.6_61_win\ae25.6_61.64bit.AfterEffectsSDK"
set "AE_HEADERS=%AE_SDK_ROOT%\Examples\Headers"
set "ORT_ROOT=C:\onnxruntime-win-x64-1.24.2"
set "ORT_INCLUDE=%ORT_ROOT%\include"
set "ORT_LIB=%ORT_ROOT%\lib\onnxruntime.lib"
set "ORT_DLL_DIR=%ORT_ROOT%\lib"
```

검증:
```cmd
if exist "%AE_HEADERS%\AE_Effect.h" (echo AE_HEADER: OK) else (echo AE_HEADER: MISSING)
if exist "%ORT_INCLUDE%\onnxruntime_cxx_api.h" (echo ORT_HEADER: OK) else (echo ORT_HEADER: MISSING)
if exist "%ORT_LIB%" (echo ORT_LIB: OK) else (echo ORT_LIB: MISSING)
if exist "%ORT_DLL_DIR%\onnxruntime.dll" (echo ORT_DLL: OK) else (echo ORT_DLL: MISSING)
```

## 4) 빌드 및 배치

CMD:
```cmd
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_aex.ps1 ^
  -AeSdkIncludeDir "%AE_HEADERS%" ^
  -OrtIncludeDir "%ORT_INCLUDE%" ^
  -OrtLibrary "%ORT_LIB%" ^
  -BuildDir "build-win" ^
  -Config Release ^
  -CopyToMediaCore
```

ORT DLL 복사:
```cmd
copy /Y "%ORT_DLL_DIR%\onnxruntime.dll" "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\onnxruntime.dll"
```

결과 확인:
```cmd
if exist "build-win\plugin\Release\ZSoda.aex" (echo BUILD_AEX: OK) else (echo BUILD_AEX: MISSING)
if exist "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\ZSoda.aex" (echo MEDIA_AEX: OK) else (echo MEDIA_AEX: MISSING)
if exist "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\onnxruntime.dll" (echo MEDIA_ORT_DLL: OK) else (echo MEDIA_ORT_DLL: MISSING)
```

## 5) AE 스모크 테스트

- AE 재시작 후 `ZSoda` 이펙트 로드 확인
- `docs\build\AE_SMOKE_TEST.md`의 ST-01 ~ ST-07 순서대로 실행
- 최소 필수 보고:
  - ST-01 (로드)
  - ST-03 (DepthMap)
  - ST-04 (Slicing)
  - ST-06 (Render Queue)

## 6) 실패 시 보고 템플릿

- 커밋: `git rev-parse --short HEAD`
- 실패 단계: (예: build/configure/AE load/ST-04)
- 명령/절차:
- 에러 원문 (전체):
- 재현 여부: 항상/간헐
- 스크린샷/로그 경로:

## 7) 운영 규칙

- 모델 가중치/대용량 바이너리는 git에 커밋하지 않는다.
- 스크립트 수정 시 `module: summary` 커밋 규칙을 따른다.
- 작업 단위 완료 후 `PROGRESS.md`를 갱신한다.
