# AGENTS.md

openBVE 호환 철도 시뮬레이터. C++20, `engine/`의 phx Vulkan 엔진 위에 만듭니다.

- 빌드: `cmake -S . -B build -G Ninja && cmake --build build --target openTrainSim`
- 런타임은 C++와 C# 플러그인 호스트(`tools/PluginHost`) 두 개뿐입니다. Lua, Python 같은 스크립트 런타임을 추가하지 않습니다. 콘텐츠는 JSON으로 씁니다.
- 기존 openBVE 형식 지원은 유지합니다.
- `data/`(노선, 열차 애드온)는 재배포 금지라 커밋하지 않습니다.
- 커밋은 요청이 있을 때만 합니다.
- 문서와 README는 한국어로 씁니다.
