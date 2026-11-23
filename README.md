# openTrainSim

openBVE 호환 철도 시뮬레이터. `engine/`의 phx 엔진(C++20 / Vulkan 1.3, 바인드리스)을 수정·확장해서 그 위에 만들었습니다.

```
cmake -S . -B build -G Ninja && cmake --build build
./build/openTrainSim "data/Railway/Route/KTX123.csv"            # 노선의 Train.Folder 열차로 시작
./build/openTrainSim route.csv --train /path/to/Train/KTX --station 0
```

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
| Ctrl+X / Ctrl+J / Pause / Ctrl+Q | 통계 / 배속 / 일시정지 / 종료 |

## 호환 범위

- **노선 (CSV)**: `$Include`(오프셋·가중치 랜덤 포함), `$Chr/$Rnd/$Sub/$If`, `With`, 트랙 위치 안정 정렬, 단위(UnitOfLength), 블록 길이. Track: Rail/RailStart/RailEnd/RailType, Curve(캔트), Pitch, Turn, Height(보간), Ground/Cycle, FreeObj(레일·지면), Wall/Dike, Pole, Form(승강장 변형 포함), Crack, Back, Fog, Brightness, Section/SigF, Limit, Sta/Stop. 지오메트리는 openBVE `ApplyRouteData`와 같은 식(블록 내 원호, 반각 회전, 보조 레일 방향)입니다.
- **오브젝트**: CSV/B3D 전체 명령, DirectX `.x` 텍스트(Frame 계층, 재질, 검정 투명), `.animated`(Include, States, Translate/Rotate/TextureShift 함수, 감쇠, RefreshRate). 함수 스크립트는 infix 전체(연쇄 비교 포함)를 바이트코드로 컴파일합니다.
- **열차**: `train.dat`(가속 곡선·지연·저크·제동통 압력·핸들·운전석·차량), `extensions.cfg`(차량 오브젝트·길이·축·연결기·반전), `panel.animated`(3D 운전실).
- **시뮬레이션**: 차량별 견인을 편성 질량으로 환산(openBVE와 동일), 주행/공기 저항, 구배, 제동통 압력 기반 감속, 폐색 구간 현시(인덱스·값 방식, 출발 신호 정지 유지), 역 정차 판정, 속도 제한.
- **사운드 (OpenAL)**: BVE4 `sound.cfg`(없으면 BVE2 기본 파일명), `train.dat` 모터음 테이블(P1/P2/B1/B2), 주행음·플랜지음(노선 `Train.Run/Flange`와 RailType), 분기기 통과음, 제동 완해·비상·제륜자, 출입문, 기적(단발/시작·반복·끝), 핸들·역전기·차단기·표시등·컴프레서, 노선 `Track.Announce`, 역 도착·출발 방송. 감쇠는 openBVE 식(반경 기반)이고, 가장 큰 소리 N개만 실제 음원에 배정하고 나머지는 가상 재생합니다. Ctrl+M(openBVE MISC_MUTE) 음소거, `[sound] volume`.
- **열차 플러그인**: `ats.cfg`의 openBVE .NET 플러그인(`OpenBveApi.Runtime.IRuntime`, 예: KTX ATP/TVM)을 Mono 별도 프로세스(`PluginHost.exe`)에서 실행합니다. Elapse·핸들 오버라이드·`pluginState[]`·신호·비콘(`Track.Beacon/Transponder`)·보안 키(S, A1…L)·메시지·플러그인 사운드를 지원합니다. 빌드에 `mcs`와 openBVE의 `OpenBveApi.dll`이 필요합니다(`-DOTS_OPENBVE_DIR=`).
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
src/sim      열차 물리, 시뮬레이션(시각·신호·역)
src/render   텍스처/재질/메시 캐시, 렌더러, HUD
src/game     게임 루프, 카메라, 입력
shaders/     메시(불투명/블렌드), 배경
tools/check  헤드리스 검사: 노선·오브젝트 전체 파싱, 주행 시험, 오브젝트 덤프, 스크립트 자가 시험
```

개발용 옵션: `--aa dlss-quality`, `--fg 1`, `--frames N --screenshot out.png`(N프레임 후 종료하며 저장), `--camera exterior:거리,yaw,pitch | cab:yaw,pitch | track`, `--autopilot`, `--timefactor N`. `ots_check route.csv [--objects]`, `ots_check --dump file.csv`, `ots_check --selftest`, `ots_check --sounds <열차 폴더>`. 환경변수: `OTS_PLUGIN_TRACE=1`(비콘·pluginState 변화 로그), `OTS_AUTOPILOT_OVER=<km/h>`(자동운전 과속 테스트), `OTS_SOUND=1`(스크립트 실행에서도 소리).

## 아직 없는 것

- sound.xml(차량별 XML 사운드), 서스펜션음, Track.Doppler, Win32 네이티브 플러그인(ATS DLL)
- 2D 계기판(panel.cfg / panel2.cfg) — 3D 운전실(panel.animated)만 그립니다
- BVE4 이미지 신호기, 동적 배경(XML), RW 형식 노선, .3ds/.mqo 오브젝트, TFO(타 열차)
- HUD 글꼴은 ASCII만 그립니다 (역명은 로마자 부분 표시)

`data/`의 경부고속선 애드온은 원작자 약관상 재배포 금지라 저장소에서 제외(`.gitignore`)했습니다.
