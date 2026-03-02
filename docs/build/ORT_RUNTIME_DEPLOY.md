# ORT Runtime Deploy (Windows .aex)

## 1) onnxruntime.dll 위치
- 기본 권장: `.aex` 파일과 **같은 폴더**에 `onnxruntime.dll` 배치
- 대안: AE 실행 시점에 검색 가능한 `PATH` 경로(권장도 낮음)
- GPU Provider(CUDA/DirectML 등) 사용 시 provider DLL도 동일 폴더 또는 검색 경로에 함께 배치

## 2) 배포 전 빠른 검증
- 아키텍처 일치 확인: `After Effects(64-bit)`, `.aex(64-bit)`, `onnxruntime.dll(64-bit)`
- 파일 존재 확인: AE 플러그인 폴더에서 `onnxruntime.dll` 존재 여부
- 의존성 확인: `dumpbin /dependents your_plugin.aex` 또는 Dependencies 도구로 누락 DLL 점검
- 런타임 로드 확인: 플러그인 초기화 로그에 ORT backend init 성공/실패 원인 기록

## 3) 일반 오류 5개
1. `LoadLibrary failed (126)`
- 원인: `onnxruntime.dll` 또는 종속 DLL 미배치
- 조치: `.aex` 인접 경로/검색 경로에 DLL 재배치

2. `Bad EXE format (193)`
- 원인: x64/x86 아키텍처 불일치
- 조치: AE/플러그인/ORT 모두 64-bit로 통일

3. `Entry Point Not Found`
- 원인: ORT DLL 버전 불일치(빌드 시 링크한 API와 런타임 DLL 불일치)
- 조치: 빌드에 사용한 ORT 버전과 동일 DLL로 교체

4. `VCRUNTIME140*.dll` 또는 `MSVCP140*.dll` 누락
- 원인: VC++ 재배포 패키지 미설치
- 조치: Microsoft Visual C++ Redistributable(x64) 설치

5. GPU provider 초기화 실패 (CUDA/DirectML)
- 원인: provider DLL/드라이버/런타임 의존성 누락
- 조치: provider DLL 동봉, 드라이버 버전 확인, 실패 시 CPU fallback 동작 검증
