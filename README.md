# openTrainSim

openBVE 호환 철도 시뮬레이터. `engine/`의 phx 엔진(C++20 / Vulkan 1.3, 바인드리스)을 수정·확장해서 그 위에 만들었습니다.

```
cmake -S . -B build -G Ninja && cmake --build build
./build/openTrainSim "data/Railway/Route/KTX123.csv"            # 노선의 Train.Folder 열차로 시작
./build/openTrainSim route.csv --train /path/to/Train/KTX --station 0
```

### Windows

Visual Studio 2022(C++ 데스크톱 개발)와 [Vulkan SDK](https://vulkan.lunarg.com/)(`glslc`)가 필요합니다. GLFW, glm, Vulkan 헤더, OpenAL Soft는 configure 때 받아서 함께 빌드합니다(OpenAL은 정적 링크). "x64 Native Tools" 명령 프롬프트에서:

```
cmake --preset windows-msvc && cmake --build --preset windows-msvc
build-windows\openTrainSim.exe "D:\BVE\Railway\Route\KTX123.csv"
```

Visual Studio의 "폴더 열기"나 CLion(MSVC 툴체인)에서는 `windows-msvc` 프리셋을 고르면 됩니다.
- **DLSS:** MSVC 빌드에도 들어갑니다. NGX 런타임 DLL이 실행 파일 옆에 복사됩니다.
- **열차 플러그인:** `PluginHost.exe`를 Mono 없이 .NET Framework 4.x로 바로 실행합니다. 빌드할 때 `csc`(.NET Framework에 포함, 자동으로 찾음)와 openBVE의 `OpenBveApi.dll`이 필요합니다(`-DOTS_OPENBVE_DIR=`, 기본값 `%USERPROFILE%\OpenBVE\bin_release`).
- **파일 이름:** 실행 파일 매니페스트가 프로세스 코드 페이지를 UTF-8로 지정합니다(Windows 10 1903 이상). 한글이나 일본어가 들어간 경로도 그대로 열립니다.
- **설정 위치:** openBVE 키 설정은 `%APPDATA%\openBVE\Settings`에서 찾습니다. glTF 텍스처 캐시는 `%LOCALAPPDATA%\openTrainSim\cache`에 둡니다.

리눅스에서 크로스 컴파일하려면 MinGW-w64(`mingw-w64-gcc` 또는 llvm-mingw)를 PATH에 두고 `cmake --preset windows-cross-mingw && cmake --build --preset windows-cross-mingw`를 실행합니다. 정적 링크라 DLL 없이 실행됩니다. llvm-mingw(libc++)에서는 OpenAL Soft 1.25 소스가 컴파일되지 않으므로, 미리 빌드한 OpenAL을 `-DOPENAL_LIBRARY= -DOPENAL_INCLUDE_DIR=`로 지정합니다. `.github/workflows/windows.yml`은 push할 때마다 MSVC로 빌드해 실행 파일을 아티팩트로 올립니다.

첫 실행 때 실행 파일 옆에 주석이 달린 `openTrainSim.cfg`가 생깁니다(시야 거리, MSAA, 텍스처 크기, 힙 크기, 컨트롤 파일 등).

## 조작 (openBVE와 동일, 변경 가능)

키 설정은 openBVE의 컨트롤 파일 형식을 그대로 읽습니다. `[controls] file`이 비어 있으면 `~/.config/OpenBve/Settings/*/controls.cfg`(본인의 openBVE 설정)를 자동으로 쓰고, 없으면 openBVE `Default.controls`와 같은 기본값을 씁니다. 1.4.4+ 이름(`Period`)과 옛 SDL 이름(`PERIOD`) 모두 읽습니다.

| 키 | 기능 |
|---|---|
| Z / A | 역행 노치 올림 / 내림 (원핸들 열차는 SINGLE_POWER / SINGLE_NEUTRAL) |
| . / , | 제동 노치 올림 / 내림 |
| / | 비상제동 |
| F / V | 역전기 전진 / 후진 |
| F5 / F6 | 좌 / 우 출입문 |
| Enter | 기적 |
| F1 / F2 / F3 / F4 | 운전실 / 외부 / 선로 / 자유 카메라 |
| 방향키, 마우스 드래그, 휠 | 시점 회전, 줌(외부: 거리, 자유: 이동 속도) |
| 키패드 2·4·6·8·9·3 | 카메라 이동, 키패드 5 리셋 |
| Esc | 설정 창: 그래픽(AA·DLSS·FSR·프레임 생성·VSync·시야 거리·FOV), 시뮬레이션, 조작 |
| F10 / F11 | HUD 숨김 / 후면 컬링 토글 |
| Ctrl+X / Ctrl+J / Pause / Ctrl+Q | 프로파일러 → 프로파일러+통계 → 끔 / 배속 / 일시정지 / 종료 |

## 그래픽

에셋은 그대로 두고 렌더러에서 화질을 올립니다. 모두 설정 창(Esc → Graphics)이나 `openTrainSim.cfg`의 `[render]`에서 끌 수 있습니다.

- **태양 그림자:** 노선의 광원 방향으로 4단 캐스케이드 섀도맵을 그립니다. 캐노피, 고가, 열차 그림자가 생기고, 운전실에서는 숨겨진 차체가 그림자를 드리워 창으로만 햇빛이 들어옵니다(`cabSunlight`). 설정은 `shadows`, `shadowResolution`, `shadowDistance`, `shadowStrength`입니다.
- **앰비언트 오클루전(SSAO):** 깊이 버퍼만으로 모서리, 틈, 물체 아래를 어둡게 합니다(`ssao`, `ssaoRadius`, `ssaoStrength`).
- **조명:** 위는 하늘빛, 아래는 지면빛인 반구 앰비언트(`hemisphereLight`)와 약한 태양 하이라이트(`specular`)를 씁니다. openBVE의 밝기 범위는 그대로 유지합니다.
- **출력:** 밝은 곳 주변에 블룸을 넣고, 약한 S-커브 대비, 채도, 비네트를 줍니다(`bloom`, `contrast`, `saturation`, `vignette`, 0이면 끔).

## VVVF 구동음 합성 (vvvf.json)

인버터 변조 패턴(비동기 캐리어, 동기 N펄스, 1펄스)을 적은 프리셋만 있으면 구동음을 실시간으로 합성합니다. 열차 폴더의 `vvvf.json`이나 `--vvvf <파일>`로 지정하고, `ots_check --vvvf <프리셋> out.wav`로 게임 없이 들어볼 수 있습니다. 자세한 내용은 `docs/vvvf.md`에 있습니다.

## 한국 열차방호장치 내장 (safety.json)

일반선 ATS(지상자 점제어식), 고속선 TVM430, 한국형 ATP(ETCS Level 1 기반)를 시뮬레이터 안에 내장했습니다. openBVE 플러그인이나 Mono 없이 동작합니다.
- **지상설비 자동 생성**: ATS 코드, TVM 단계, ATP 이동권한을 폐색 점유와 현시에서 만듭니다. 노선에 비콘을 깔 필요가 없어서 openBVE 노선에서도 그대로 동작합니다.
- **열차 설정**: 열차 폴더의 `safety.json`에 장치와 값을 적습니다.
- **구역 전환**: 장치 구역이 바뀌면 운전실에서 확인을 요구합니다.
- **표시와 소리**: 왼쪽 아래 DMI(원형 속도계, 허용·개입 속도 띠, 목표 거리)와 합성 경보음이 나옵니다.
- **운전실 연결**: `cab.json` 함수식에서 `safety[n]`으로 값을 받습니다.

자세한 내용은 [docs/safety.md](docs/safety.md)를 보세요.

## 자체 노선 형식 (.route.json)

선형, 구배, 역, 폐색 신호, 속도 제한, 보안장치 구역을 JSON으로 적으면 선로, 신호기, 승강장이 코드로 만들어집니다. 외부 오브젝트가 필요 없습니다. 시험 노선 `examples/routes/safety-test.route.json`(20 km, ATS → ATP → TVM430 구역)과 시험 열차 `examples/trains/test-emu`가 함께 들어 있습니다. 형식은 [docs/route-format.md](docs/route-format.md)에 있습니다.

```
./build/openTrainSim examples/routes/safety-test.route.json
./build/ots_check --safety examples/routes/safety-test.route.json examples/trains/test-emu obey examples/routes/safety-test.traffic.json
```

## 다른 열차 (traffic.json)

노선에 여러 열차가 함께 달립니다. 모든 열차가 같은 폐색 구간을 쓰므로, 앞 열차가 있는 구간은 정지 현시가 되고 뒤 구간 신호가 차례로 바뀝니다. 노선의 `Route.RunInterval`(openBVE와 같은 선행 열차)을 읽습니다. 노선 옆 `<노선명>.traffic.json`에는 선행·후속 열차, 다른 차종, 출발 시각, 통과 운행을 적을 수 있습니다. AI 열차는 속도 제한, 신호, 시간표를 지키고 역에 정차합니다. 자세한 내용은 [docs/traffic.md](docs/traffic.md)를 보세요. 예제는 `examples/traffic/`에 있습니다.

## 하늘과 시간 (물리 기반 하늘)

노선의 배경 이미지 대신, 날짜·시각·위치로 계산한 실제 하늘을 그립니다(`[sky]`, `enabled = 0`이면 openBVE 배경 이미지).

- **태양과 달:** 날짜(`date`, 비우면 오늘), 위도·경도(`latitude`, `longitude`), 표준시(`utcOffset`)로 위치를 계산합니다. 노선의 +z 방향이 북쪽이 아니면 `northAzimuth`로 돌립니다.
- **대기 산란:** Rayleigh·Mie·오존 단일 산란에 다중 산란 근사를 더했습니다. 지구 그림자 덕분에 노을 → 어두운 노을 → 시민·항해·천문 박명 → 밤이 순서대로 이어지고, 눈의 순응처럼 노출이 서서히 올라갑니다.
- **조명:** 태양(밤에는 달)의 방향과 색, 하늘빛, 안개 색이 같은 모델에서 나옵니다. 정오에는 노선 원래의 조명값 그대로입니다.
- **별:** NASA SVS Deep Star Maps 2020(적도 좌표)과 ESA Gaia EDR3 은하수 지도(은하 좌표)를 항성시에 맞춰 돌려 투영합니다. 에셋은 재배포하지 않으므로 `tools/fetch_sky.sh`로 `data/sky/`에 받습니다(약 140 MB). 없으면 별 없이 하늘만 그립니다.

## 지형 (terrain.json)

노선 옆 `<노선명>.terrain.json`에 노선이 지도의 어디인지(역 이름 + 위도·경도)만 적으면, 실제 표고 자료로 선로 주변 지형을 만듭니다. 땅깎기와 흙쌓기, 갱구와 라이닝을 갖춘 터널, 고가 아래 골짜기, 바다도 만듭니다. 노선 오브젝트가 있는 폭만큼은 땅을 평평하게 두어 오브젝트가 묻히지 않습니다. 노선이 직접 그린 터널(`tunnel` 오브젝트)은 그 위만 지형으로 덮습니다. 표고 타일은 처음 한 번만 받아 캐시합니다. 자세한 내용은 [docs/terrain.md](docs/terrain.md)를 보세요. 예제는 `examples/terrain/`에 있습니다.

## 3D 운전실 (cab.json)

운전실은 3D 모델(glTF 2.0, `.glb`)로 만듭니다. Blender 등에서 운전실을 모델링해 내보내고, 열차 폴더의 `cab.json`에 노드 이름으로 레버, 노브, 버튼, 계기 바늘, 램프를 지정합니다. 스크립트 언어는 쓰지 않습니다. 각 부품은 모델에서 잡은 원점을 축으로 움직입니다. 마우스로 클릭, 드래그, 휠 조작을 하고, 가리킨 부품은 밝아지며 이름이 표시됩니다.

- 부품은 핸들, 역전기, 출입문, 기적, 보안 키, openBVE 명령에 연결할 수 있습니다.
- 계기 값과 램프 조건은 openBVE 함수식(panel.animated와 같은 문법)으로 쓰고, 복잡한 로직은 플러그인 값(`pluginState`)으로 받습니다.
- glTF 말고도 csv/b3d/x/.animated 파일을 부품 모델로 쓸 수 있습니다.

`cab.json`이 없으면 기존 panel.animated를 씁니다. API는 [docs/cab-api.md](docs/cab-api.md), 예제는 `examples/cab/`(`cab.json` + 시험용 모델 `desk.glb`, `--cab examples/cab/cab.json`)에 있습니다.

## 호환 범위

- **노선 (CSV)**: `$Include`(오프셋·가중치 랜덤 포함), `$Chr/$Rnd/$Sub/$If`, `With`, 트랙 위치 안정 정렬, 단위(UnitOfLength), 블록 길이. Track: Rail/RailStart/RailEnd/RailType, Curve(캔트), Pitch, Turn, Height(보간), Ground/Cycle, FreeObj(레일·지면), Wall/Dike, Pole, Form(승강장 변형 포함), Crack, Back, Fog, Brightness, Section/SigF, Limit, Sta/Stop. 지오메트리는 openBVE `ApplyRouteData`와 같은 식(블록 내 원호, 반각 회전, 보조 레일 방향)입니다.
- **오브젝트**: CSV/B3D 전체 명령, DirectX `.x` 텍스트(Frame 계층, 재질, 검정 투명), `.animated`(Include, States, Translate/Rotate/TextureShift 함수, 감쇠, RefreshRate). 함수 스크립트는 infix 전체(연쇄 비교 포함)를 바이트코드로 컴파일합니다.
- **열차**: `train.dat`(가속 곡선·지연·저크·제동통 압력·핸들·운전석·차량), `extensions.cfg`(차량 오브젝트·길이·축·연결기·반전), `panel.animated`(3D 운전실).
- **시뮬레이션**: 차량별 견인을 편성 질량으로 환산(openBVE와 동일), 주행/공기 저항, 구배, 제동통 압력 기반 감속, 폐색 구간 현시(인덱스·값 방식, 출발 신호 정지 유지, 모든 열차의 점유 반영), 역 정차 판정, 속도 제한, `Route.RunInterval`/`Train.Interval` 선행 열차(AI 운전).
- **사운드 (OpenAL)**: BVE4 `sound.cfg`(없으면 BVE2 기본 파일명), `train.dat` 모터음 테이블(P1/P2/B1/B2), 주행음·플랜지음(노선 `Train.Run/Flange`와 RailType), 분기기 통과음, 제동 완해·비상·제륜자, 출입문, 기적(단발/시작·반복·끝), 핸들·역전기·차단기·표시등·컴프레서, 노선 `Track.Announce`, 역 도착·출발 방송. 감쇠는 openBVE 식(반경 기반)이고, 가장 큰 소리 N개만 실제 음원에 배정하고 나머지는 가상 재생합니다. Ctrl+M(openBVE MISC_MUTE) 음소거, `[sound] volume`.
- **열차 플러그인**: `ats.cfg`의 openBVE .NET 플러그인(`OpenBveApi.Runtime.IRuntime`, 예: KTX ATP/TVM)을 별도 프로세스(`PluginHost.exe`, 리눅스는 Mono, Windows는 .NET Framework)에서 실행합니다. Elapse·핸들 오버라이드·`pluginState[]`·신호·비콘(`Track.Beacon/Transponder`)·보안 키(S, A1…L)·메시지·플러그인 사운드를 지원합니다. 빌드에 C# 컴파일러(`mcs` 또는 `csc`)와 openBVE의 `OpenBveApi.dll`이 필요합니다(`-DOTS_OPENBVE_DIR=`).
- **추가 키 설정**: 기본 키맵(또는 내 openBVE `controls.cfg`) 위에 열차 폴더의 `controls.cfg`·`*.controls`, 노선 옆 `<노선명>.controls`를 덧붙입니다(같은 키만 덮어씀).
- **텍스트 인코딩**: UTF-8/UTF-16 BOM 감지, 아니면 CP949(설정 가능)로 디코드. 윈도 경로는 대소문자를 무시하고 해석합니다.

## 렌더러와 메모리

| 기법 | 효과 (경부고속선 KTX123, 실측) |
|---|---|
| 오브젝트는 GPU에 한 번만 올리고 모든 배치를 인스턴싱 (openBVE는 배치마다 복제) | 426 km · 107,442 배치의 고유 지오메트리 전체가 ≈ 21 MB, 시야 내는 5–10 MB |
| 텍스처 BC1/BC3 블록 압축 + 밉맵(알파 가중, 투명 텍셀 색 번짐 방지, 알파테스트 커버리지 보존) | 시야 내 텍스처 142 MB (RGBA8였다면 992 MB, 약 7배 절감) |
| 24바이트 정점(옥타헤드럴 법선) + 16비트 인덱스(65535 초과 시 자동 분할) | 정점 메모리 ≈ 40% 절감 |
| 큰 정점/인덱스 힙 하나를 VMA 가상 블록으로 서브할당, 해제는 프레임 지연 | 메시당 VkBuffer/할당 없음 |
| 거리 기반 스트리밍 + LRU 퇴출, CPU 메시는 업로드 직후 해제 | 90 km 주행 중 VRAM 370–480 MB로 일정 |
| 파싱·디코드·압축은 워커 스레드, 메인은 프레임당 업로드 예산 안에서 복사만 | 300 km/h에서도 대기 0–1건, 끊김 없음 |
| 작업 우선순위 -1(즉시)~5: 운전 열차·운전실 0, 카메라 300 m 안 1, 시야 안 2, 시야 밖 한 단계 아래, 먼 선읽기 4~5. 선읽기는 워커 하나를 남겨 두고, 큐에 있는 작업은 보이면 우선순위가 올라감 | 로딩 화면은 시야 안(2)까지만 기다림 (1.9 → 1.6 s), 나머지는 주행 중 뒤에서 로드 |
| 배치는 블록 기준 float 상대좌표 + 쿼터니언(44 B), 행렬은 보이는 것만 카메라 상대로 계산 | 400 km 넘는 노선에서도 정밀도 문제 없음 |
| 엔진 후처리 파이프라인(README 순서): 렌더 해상도 HDR + 깊이 + 모션벡터 → DLSS SR/DLAA 또는 TAA/FSR 1/SSAA → 출력 변환(+FXAA) → UI → DLSS 프레임 생성 | 설정창에서 즉시 전환, DLSS 불가 GPU는 FSR/TAA로 대체 |

## 구조

```
engine/                phx 엔진 (수정됨)
  phx/gpu/BlockCompress  [추가] 밉맵 + BC1/BC3 압축
  phx/gpu/Upload         [추가] 프레임 단위 스테이징 업로드 큐, 원소 단위 GPU 힙
  phx/gpu/Resources      [수정] 바인드리스 16384 슬롯 + 슬롯 할당기(지연 재사용), MSAA/A2C/전면 방향/블렌드 모드
  phx/gpu/VulkanContext  [수정] descriptorBindingUpdateUnusedWhilePending
  phx/gpu/Textures       [수정] BMP/GIF/TGA 디코드
src/util     인코딩, 경로 해석, 설정, 스레드풀
src/bve      노선/오브젝트/열차/컨트롤 파서, 함수 스크립트
src/sim      열차 물리, 시뮬레이션(시각·신호·역), AI 열차(traffic.json)
src/safety   내장 열차방호장치: Safety(지상설비 뷰·차상 묶음·설정), Ats, Tvm430, Atp
src/route    자체 노선 형식(.route.json) → Route, 절차적 선로·신호기·승강장
src/terrain  DEM 지형: Geo(지도 맞춤·DEM 타일), Track(선로 공간 색인), Model(노반·높이·터널/교량 판정), Mesh(타일·라이닝·갱구), Terrain(설정·조립)
src/render   텍스처/재질/메시 캐시, 렌더러, HUD
src/cab      3D 운전실 (cab.json: glTF 노드 → 레버/버튼/계기/램프, 마우스 조작, 열차 바인딩)
src/game     게임 루프, 카메라, 입력
shaders/     메시(불투명/블렌드), 배경
tools/check  헤드리스 검사: 노선·오브젝트 전체 파싱, 주행 시험, 오브젝트 덤프, 스크립트 자가 시험
```

개발용 옵션: `--aa dlss-quality`, `--fg 1`, `--frames N --screenshot out.png`(N프레임 후 종료하며 저장), `--camera exterior:거리,yaw,pitch | cab:yaw,pitch | track`, `--autopilot`, `--timefactor N`, `--cab <cab.json|none>`, `--cab-click u,v,프레임`(운전실 요소 클릭 시험, u·v는 -1~1). `ots_check route.csv [--objects]`, `ots_check --dump file.csv`, `ots_check --selftest`, `ots_check --sounds <열차 폴더>`, `ots_check --traffic <노선> <열차 폴더> <traffic.json> [분] [stay|drive]`, `ots_check --terrain <노선> <terrain.json> [타일 수]`(지형을 화면 없이 만들고 메시 생성 시간과 체크섬 출력). `ots_check --safety <노선> <열차 폴더> <obey|ack|ignore|redpass> [traffic.json|none] [분]`(내장 보안장치 시나리오). `--traffic <파일|none>`, `--terrain <파일|none>`, `--time HH:MM[:SS]`(시작 시각). 환경변수: `OTS_TRAFFIC_TRACE=1`(AI 열차 정지 이유), `OTS_TERRAIN_DEBUG=1`(터널 목록·높이 계산), `OTS_START_TRACK=<m>`(그 거리정에서 시작), `OTS_PLUGIN_TRACE=1`(비콘·pluginState 변화 로그), `OTS_AUTOPILOT_OVER=<km/h>`(자동운전 과속 테스트), `OTS_SOUND=1`(스크립트 실행에서도 소리).

## 아직 없는 것

- sound.xml(차량별 XML 사운드), 서스펜션음, Track.Doppler, Win32 네이티브 플러그인(ATS DLL)
- 2D 계기판(panel.cfg / panel2.cfg) — 3D 운전실(panel.animated)만 그립니다
- BVE4 이미지 신호기, 동적 배경(XML), RW 형식 노선, .3ds/.mqo 오브젝트, TFO(타 열차)
- AI 열차의 반대 방향 운행, 다른 선로로 대피·추월, 주행음

`data/`의 경부고속선 애드온은 원작자 약관상 재배포 금지라 저장소에서 제외(`.gitignore`)했습니다.

## 라이선스

GNU General Public License v3.0 이상(`LICENSE`). 사용한 서드파티 라이브러리와 각 라이선스는 `THIRD_PARTY_NOTICES.md`를 보세요. 노선·열차·플러그인 애드온은 각 원작자의 것이며 이 라이선스의 대상이 아닙니다.
