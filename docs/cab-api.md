# 3D 운전실 (cab.json)

운전실은 3D 모델로 만듭니다. Blender 같은 도구에서 운전실 전체를 모델링해 glTF 2.0(`.glb` / `.gltf`)으로 내보내고, 열차 폴더의 `cab.json`에 모델의 어떤 노드가 무슨 조작기인지 적습니다.

- 형상, 재질, 텍스처, 회전축은 모두 모델에서 가져옵니다. `cab.json`에는 연결과 동작만 적습니다.
- 스크립트 언어는 쓰지 않습니다. 계기 값과 조건은 openBVE 함수식(panel.animated와 같은 문법)으로 쓰고, 복잡한 로직은 열차 플러그인(ATS/ATP)에서 처리한 뒤 `pluginState[n]`으로 받습니다.
- 운전사는 마우스로 조작합니다(클릭, 드래그, 휠). 키보드 조작도 그대로 되고, 연결된 레버와 버튼이 키 입력을 따라 움직입니다.

`cab.json`이 없으면 기존 openBVE 운전실(panel.animated)을 씁니다. 다른 파일을 시험하려면 `--cab <파일>`, 끄려면 `--cab none`을 줍니다. 설정 파일에서는 `[simulation] cab3d = 0`으로 끕니다.

예제는 `examples/cab/`에 있습니다(`cab.json`, `desk.glb`). `desk.glb`는 시험용 모델이고 `tools/example_cab_glb.py`가 만듭니다.

```
openTrainSim <route.csv> --cab examples/cab/cab.json
```

## 모델링 규칙

1. **원점.** 차량 좌표(차량 중심이 원점, x 오른쪽, y 위, z 앞) 그대로 모델링하거나, 운전사 눈 기준으로 모델링하고 `"origin": "driver"`로 배치합니다.
2. **움직이는 부품은 따로 둡니다.** 레버, 노브, 버튼, 바늘은 각각 별도의 오브젝트(노드)로 만들고 이름을 붙입니다(예: `MasterLever`, `Needle_Speed`).
3. **오브젝트 원점이 회전축입니다.** 레버는 축 위치에, 바늘은 계기 중심에, 버튼은 밑면에 원점을 둡니다(Blender: Set Origin). 회전과 이동은 이 원점과 오브젝트의 로컬 축을 기준으로 합니다.
4. **자식 노드는 함께 움직입니다.** 레버 손잡이를 레버의 자식으로 두면 같이 돕니다. 자식이 따로 요소로 지정되면 부모에서는 빠지고 자기 요소로 움직입니다.
5. **상태가 있는 부품**(램프 꺼짐/켜짐, 표시기)은 같은 위치에 상태별 노드를 하나씩 둡니다(`Lamp_EB_Off`, `Lamp_EB_On`). 빛나는 상태에는 Emission을 줍니다.
6. **클릭 범위.** 작은 부품에는 보이지 않을 상자 노드를 자식으로 두고 `hit`으로 지정합니다. 지정된 hit 노드는 그려지지 않습니다.
7. **내보내기.** glTF 2.0 바이너리(`.glb`)를 권장합니다. 텍스처가 파일 안에 들어갑니다. +Y Up(기본값)으로 내보내고, 열차 진행 방향이 glTF +Z(Blender −Y)를 향하게 합니다. Apply Modifiers를 켭니다. Draco 압축은 지원하지 않습니다.

지원하는 재질 속성은 Base Color(색, 텍스처), Emission(세기 포함), Alpha(Blend), Double Sided, Unlit입니다. 금속성, 거칠기, 노멀 맵은 아직 쓰지 않습니다.

`.glb` / `.gltf`는 운전실 밖에서도 일반 오브젝트로 쓸 수 있습니다. 장면 전체가 하나의 정적 오브젝트가 됩니다.

## 파일 형식

JSON입니다. 직접 편집하기 편하도록 `//`, `/* */` 주석과 끝에 붙은 쉼표를 허용합니다.

```jsonc
{
  "name": "KTX-산천 운전실",
  "model": { "file": "cab.glb" },               // 또는 "models": [ {...}, {...} ]
  "eye": { "offset": [0, 0.05, 0] },            // 선택: 시점 (train.dat 운전사 위치 기준, 또는 [x,y,z] 차량 좌표)
  "body": ["extra.animated"],                   // 선택: 함께 그릴 openBVE 오브젝트
  "elements": [ ... ],
  "alerts": [ ... ]
}
```

### model

| 필드 | 설명 |
|---|---|
| `file` | glTF 파일(cab.json 기준 경로). 문자열만 줘도 됩니다: `"model": "cab.glb"` |
| `origin` | `"car"`(기본): 모델 좌표 = 차량 좌표. `"driver"`: 모델 원점을 train.dat의 운전사 눈 위치에 둡니다. |
| `at` | 추가 이동 `[x, y, z]` (m) |
| `rotation` | `[rx, ry, rz]` (도, x → y → z 순서) |
| `scale` | 숫자 또는 `[sx, sy, sz]` |
| `hide` | 그리지 않을 노드 이름 목록(보조 오브젝트 등) |
| `body` | `false`면 요소만 쓰고 나머지는 그리지 않습니다(기본 `true`). |

요소가 가져가지 않은 나머지 노드는 모두 운전실 본체로 그려집니다.

### elements

```jsonc
{ "type": "lever", "node": "MasterLever", "hit": "MasterLever_Hit", "tooltip": "주간제어기",
  "bind": "onehandle", "axis": "x", "angles": [-40, 40] }
```

| 필드 | 설명 |
|---|---|
| `type` | `button`, `switch`, `lever`, `gauge`, `lamp`, `part` |
| `node` | 이 노드(와 자식)가 요소의 모양이 됩니다. 위치와 방향도 노드에서 가져옵니다. |
| `nodes` | 상태별 노드 `["끔", "켬", ...]` (`null` = 아무것도 안 보임). 첫 노드의 원점이 축입니다. |
| `hit` | 클릭 범위 노드 이름(그려지지 않음), 또는 `{ "size": [w,h,d], "at": [x,y,z] }` |
| `from` | 모델이 여럿일 때 모델 번호 또는 파일 이름(생략하면 그 노드가 있는 첫 모델) |
| `name`, `tooltip` | 이름(기본: 노드 이름)과 마우스를 올렸을 때 보이는 설명 |
| `bind` | 열차와 연결(아래 표) |
| `value` | openBVE 함수식(`"speedometer * 3.6"`, `"pluginState[73]"`) 또는 초깃값 숫자 |
| `min`, `max` | 값 범위. `bind`가 정해 주면 생략합니다. 스위치는 `positions`로 위치 수를 줄 수 있습니다. |
| `axis`, `angles` | 값에 따른 회전. 축은 노드의 로컬 축(`"x"`, `"-y"`, `[x,y,z]`)이고, 각도(도)는 min~max에 고르게 나눠 보간합니다. 숫자 하나면 min에서 0, max에서 그 값입니다. |
| `direction`, `offsets` / `travel` | 값에 따른 이동(m). 버튼 기본값은 `-y` 방향 0.004 m입니다. |
| `damping` | 표시 지연(초). 바늘과 핸들의 부드러운 움직임에 씁니다. |
| `toggle` | 버튼: 누를 때마다 켜짐/꺼짐이 바뀝니다. |
| `spring` | 레버·스위치: 놓으면 이 값으로 돌아갑니다. |
| `wrap` | 스위치: 끝에서 처음으로 돌아갑니다. |
| `continuous` | 정수로 반올림하지 않습니다(계기는 기본 `true`). |
| `drag`, `drag_pixels` | 레버 드래그 방향(`up`/`down`/`left`/`right`)과 한 단 당 픽셀(기본 30) |
| `sound`, `sound_release` | 운전사가 값을 바꿨을 때(값이 줄 때) 나는 소리 |
| `model`, `pos`, `rotation`, `pivot` | glTF 노드 대신 별도 파일(csv / b3d / x / glb)을 쓸 때의 모델(상태별 목록 가능)과 배치. `pivot`은 노드 원점 대신 쓸 축 위치(차량 좌표)입니다. |

| 종류 | 조작 |
|---|---|
| `button` | 누르는 동안 1(`toggle`이면 누를 때마다 바뀜) |
| `switch` | 클릭: 다음 위치, 우클릭: 이전 위치. 드래그와 휠도 됩니다. |
| `lever` | 드래그와 휠 |
| `gauge`, `lamp`, `part` | 조작 없음. `value`나 `bind`를 따라 움직입니다. `lamp`는 값이 상태 번호입니다. |

### bind

| bind | 범위 | 동작 |
|---|---|---|
| `power` / `brake` | 0~최대(brake는 최대+1 = 비상) | 역행 / 제동 핸들 |
| `onehandle` (`master`) | -(제동+1)~역행 | 원핸들 주간제어기. 음수는 제동, 맨 끝은 비상입니다. |
| `reverser` | -1~1 | 역전기 |
| `emergency` | 0/1 | 비상제동 |
| `horn1` `horn2` `horn3` | 0/1 | 기적(누르는 동안), 음악 기적 |
| `doors_left` `doors_right` | 0/1 | 버튼이면 누를 때마다 열림/닫힘이 바뀌고, 스위치면 위치가 곧 열림/닫힘입니다. |
| `headlights` `hold_brake` `const_speed` | 0/1 | 해당 기능 켜기/끄기 |
| `key:A1` … `key:L`, `key:S` | 0/1 | 보안장치 키(플러그인 KeyDown/KeyUp) |
| `command:DOORS_LEFT` 등 | 0/1 | 누를 때 openBVE 명령 실행 |

### value 식

panel.animated와 같은 openBVE 함수식입니다. 자주 쓰는 변수는 다음과 같습니다.

- 속도·제한: `speedometer`(m/s), `routeLimit`(m/s)
- 공기압(Pa): `brakeCylinder`, `mainReservoir`, `brakePipe`, `straightAirPipe`
- 핸들: `powerNotch`, `brakeNotch`, `brakeNotches`, `reverserNotch`, `emergencyBrake`
- 출입문: `doors`, `leftDoors`, `rightDoors`
- 기타: `headlights`, `time`, `pluginState[n]`
- 함수: `if[조건, 참, 거짓]`, `mod[a, b]`, `abs[x]`, `min[a, b]`, `max[a, b]`
- `value`는 이전 결과입니다.

예: 깜빡이는 과속등은 `"if[speedometer > routeLimit, mod[time, 1] < 0.5, 0]"`입니다.

### alerts

조건이 참이 되는 순간 메시지와 소리를 냅니다.

```jsonc
{ "when": "speedometer > routeLimit + 1.4", "message": "과속!", "seconds": 3, "sound": "warn.wav", "repeat": 5 }
```

`repeat`(초)를 주면 조건이 계속 참인 동안 그 간격으로 다시 냅니다.

## 예정

- 화면(display) 요소: 모델의 화면 면에 그리는 동적 텍스처(ATP, TCMS 화면). 내용은 플러그인 값으로 채웁니다.
- 요소에 키보드 키 직접 연결
- 요소별 소리 반경과 반복 소리
