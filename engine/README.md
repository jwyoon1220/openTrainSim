# phx 엔진

Phanerozoic에서 떼어 낸, 다른 게임에서도 쓸 수 있는 C++20 / Vulkan 1.3 엔진입니다. 게임 고유 코드(지구·하늘·지형·식생·플레이어)는 들어 있지 않습니다.

| 모듈 | CMake 타깃 | 내용 |
|---|---|---|
| `phx/core` | `phx::core` | 정수·실수 타입, 로그, `TripleBuffer`, PNG 저장 |
| `phx/jobs` | `phx::jobs` | 워크 스틸링 잡 시스템 (GPU 의존 없음) |
| `phx/ui` | `phx::ui` | GUI 라이브러리: 창·탭·폼/플로/보더 레이아웃·버튼·체크박스·슬라이더·콤보박스 → 사각형 정점 (GPU·GLFW 의존 없음) |
| `phx/platform` | `phx::engine` | GLFW 창, 입력 수집, `UiInput`(GLFW 입력 → `phx::ui`) |
| `phx/gpu` | `phx::engine` | `VulkanContext`, `Swapchain`(+ NVIDIA Reflex), `Bindless`, 버퍼·이미지·파이프라인, 텍스처 로딩, GPU 타이머 |
| `phx/render` | `phx::engine` | `Upscaler`(DLSS SR / DLAA / 프레임 생성), `AntiAliasing`(TAA, FSR 1, SSAA, FXAA), `Bloom`, `UiRenderer` |

## 게임에 붙이기

엔진 디렉터리는 독립 CMake 프로젝트입니다. 저장소에 복사하거나 `git subtree` / 서브모듈로 넣은 뒤:

```cmake
cmake_minimum_required(VERSION 3.28)
project(MyGame LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)

add_subdirectory(engine)                       # 또는 FetchContent

add_executable(MyGame src/main.cpp ...)
phx_engine_executable(MyGame)                  # phx::engine 링크 + 엔진 셰이더 + DLSS 런타임 복사

file(GLOB MY_SHADERS CONFIGURE_DEPENDS shaders/*.vert shaders/*.frag shaders/*.comp)
phx_add_shaders(my_shaders SOURCES ${MY_SHADERS} INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
add_dependencies(MyGame my_shaders)
```

- 셰이더는 `build/shaders/`(게임)와 `build/shaders/phx/`(엔진)에 SPIR-V로 컴파일되고, `loadSpirv("phx/taa.comp")`처럼 이름으로 읽습니다. 배포할 때는 `setShaderDirectory()`로 위치를 바꿉니다.
- 엔진 단독 빌드: `cmake -S engine -B build-engine && cmake --build build-engine` (테스트와 `phx_ui_demo` 예제 포함).
- 최소 사용 예: [`examples/ui_demo.cpp`](examples/ui_demo.cpp). 창, 스왑체인, UI 창을 띄우고, DLSS를 조사하고, AA·블룸 모듈을 만듭니다.

| CMake 옵션 | 기본값 | 설명 |
|---|---|---|
| `PHX_DLSS` | ON | NGX SDK를 configure 때 받아 링크(Linux x86_64). 실패하거나 끄면 DLSS 없이 빌드(`PHX_HAS_DLSS` 미정의) |
| `PHX_BUILD_TESTS` | 단독 빌드일 때 ON | `phx_job_tests`, `phx_ui_tests` |
| `PHX_BUILD_EXAMPLES` | ON | `phx_ui_demo` |
| `PHX_SHADER_OUTPUT_DIR` | `${CMAKE_BINARY_DIR}/shaders` | SPIR-V 출력 위치 (`loadSpirv` 기본 경로) |

## 바인드리스 규약

모든 파이프라인은 `Bindless` 디스크립터 세트 하나와 128 B 푸시 상수 범위(`VK_SHADER_STAGE_ALL`)를 공유합니다. 푸시 상수의 의미는 셰이더마다 스스로 선언합니다. 게임 셰이더는 `#include "phx/bindless.glsl"`로 디스크립터 배열과 엔진 슬롯 이름을 가져옵니다.

엔진은 **샘플 슬롯 1000–1023, 스토리지 슬롯 48–63**을 예약합니다(`phx/render/PostSlots.hpp` ↔ `shaders/phx/post_slots.glsl`). 나머지는 게임이 씁니다. 게임이 채워야 하는 입력은 다음과 같습니다.

| 슬롯 | 누가 씀 | 내용 |
|---|---|---|
| `kTexHdr` | 게임 | 출력 해상도 HDR 장면 (블룸·톤매핑 입력) |
| `kTexDepth` | 게임 | 렌더 해상도 깊이, reverse-Z (TAA) |
| `kTexMotion` / `kStorMotion` | 게임 | 렌더 해상도 모션 벡터, 이전 − 현재 (UV 단위) |
| `kTexScene`, `kTexHistory*`, `kTexEasu`, `kTexLdr`, `kTexBloom*` … | 엔진 | AA·블룸 중간 결과 |

## 후처리 연결 순서

`game/render/Renderer.cpp`가 완전한 참조 구현입니다(모드 전환, DLSS 실패 시 대체, 프레임 생성, Reflex 포함). 한 프레임의 흐름은 다음과 같습니다.

1. **모드 → 해상도**: `Upscaler::renderExtent()`(DLSS) 또는 `AntiAliasing::renderExtent()`로 렌더 해상도를 정합니다. DLSS가 없으면 `fallbackWithoutDlss()`로 FSR/TAA를 씁니다.
2. **지터·밉 바이어스**: 템포럴 모드에서는 `jitterHalton()`으로 투영을 흔들고, `AntiAliasing::mipBias()`(DLSS는 `log2(render/display) − 1`)를 텍스처 샘플링에 적용합니다.
3. 장면을 렌더 해상도 HDR + 깊이로 그리고 모션 벡터를 씁니다.
4. **업스케일 / AA**: DLSS면 `Upscaler::evaluate()`, TAA·FSR·SSAA면 `AntiAliasing::resolve()`. 둘 다 출력 해상도 HDR 이미지를 씁니다.
5. **블룸**: `Bloom::record()` 뒤 톤매퍼가 `kTexBloom0`을 섞습니다.
6. **톤매핑 → 스왑체인**: FXAA 모드면 `AntiAliasing::recordFxaaInput(cmd, drawTonemap)`으로 LDR 이미지에 톤매핑한 뒤 `fxaaPipeline()`을 그립니다.
7. **UI**: 같은 패스 안에서 `UiRenderer::record()`. 프레임 생성을 켜면 `Upscaler::evaluateFrameGen()`에 UI 없는 화면·UI 포함 화면·UI 단독 이미지를 넘깁니다.

`UpscaleMode`, `upscaleModeName()`, `parseUpscaleMode()`, `dlssAvailableHint()`, `frameGenMaxHint()`는 설정 UI를 만들 때 씁니다.

## UI

```cpp
ui::Ui gui;
ui::Window& w = gui.addWindow("Settings");
w.content().add<ui::Button>("Resume").addActionListener([&] { w.close(); });
// 매 프레임
for (const InputEvent& e : window.drainInput().events) feedUi(gui, e);
gui.setScreenSize(width, height);
if (gui.build(vertices)) uiRenderer.upload(slot, vertices);   // 바뀌었을 때만 다시 만듦
uiRenderer.record(cmd, slot, extent);                         // 스왑체인 패스 안에서
```

`phx::ui`는 GPU와 무관하므로 단위 테스트(`tests/UiTests.cpp`)로 검증하고, 정점(`ui::Vertex`, 사각형당 4개, 픽셀 좌표)은 다른 스레드로 그대로 넘길 수 있습니다.

## 라이선스 메모

- DLSS: NVIDIA RTX SDKs 라이선스(SDK의 `LICENSE.txt`). 런타임 `libnvidia-ngx-dlss*.so`는 실행 파일과 함께 배포할 수 있고, 라이선스와 RTX UI 가이드라인에 따라 NVIDIA DLSS를 표기해야 합니다.
- FSR 1: AMD FidelityFX(MIT). volk·VMA·GLFW·stb는 각자의 라이선스를 따릅니다.
