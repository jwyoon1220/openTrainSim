# 자체 노선 형식 (`<이름>.route.json`)

openBVE CSV 노선과 별도로, 노선을 데이터로 적는 openTrainSim의 형식입니다. 선형, 구배, 역, 폐색 신호, 속도 제한, 열차방호장치 구역을 적으면 선로(레일·침목·도상), 신호기, 승강장이 코드로 만들어집니다. 외부 오브젝트 파일 없이 바로 달릴 수 있습니다. 시뮬레이션, 렌더러, AI 열차, 보안장치는 openBVE 노선과 똑같이 동작합니다.

파일 이름이 `.route.json`으로 끝나면 이 형식으로 읽습니다.

```
openTrainSim examples/routes/safety-test.route.json
```

옆 파일도 같은 규칙으로 찾습니다: `safety-test.traffic.json`, `safety-test.terrain.json`.

지금은 v0입니다. 현실적인 그래픽(PBR 재질, 하늘, 지형·식생, 전차선 자동 생성)은 이 형식을 바탕으로 확장합니다.

## 예

```jsonc
{
  "name": "보안장치 시험선",
  "length": 20000,                       // m
  "train": "../trains/test-emu",         // 기본 열차 폴더 (이 파일 기준)
  "startTime": "09:00:00",               // 없으면 첫 정차역 도착 시각
  "curves": [                            // at부터 다음 항목까지. radius 양수 = 오른쪽, 0 = 직선. cant는 mm
    { "at": 0, "radius": 0 },
    { "at": 2500, "radius": 1200, "cant": 100 },
    { "at": 3500, "radius": 0 }
  ],
  "grades": [ { "at": 0, "permille": 0 }, { "at": 4000, "permille": 6 } ], // 오르막 양수
  "speedLimits": [ { "at": 0, "kmh": 60 }, { "at": 600, "kmh": 110 } ],
  "stations": [
    { "name": "시험", "at": 300, "departure": "09:01:00", "platform": "right" },
    { "name": "종착", "at": 19500, "arrival": "09:17:00", "terminal": true }
  ],
  "signals": { "aspects": [0, 1, 2, 3, 4], "spacing": 1200, "side": "left" },
  "safety": [ { "at": 0, "system": "ats" }, { "at": 6000, "system": "atp" } ],
  "environment": { "sunElevation": 42, "sunAzimuth": 150 }
}
```

## 항목

| 키 | 뜻 |
|---|---|
| `length` | 노선 길이 (m, 필수) |
| `train` | 기본 열차 폴더. `--train`으로 바꿀 수 있습니다 |
| `startTime` | 시작 시각 `HH:MM[:SS]` |
| `curves[]` | `at`, `radius`(m, 양수 오른쪽), `cant`(mm). 25 m 블록마다 적용합니다 |
| `grades[]` | `at`, `permille` |
| `speedLimits[]` | `at`, `kmh` (없으면 제한 없음) |
| `stations[]` | `name`, `at`(열차 앞이 서는 위치), `arrival`, `departure`, `stopSeconds`, `pass`(통과), `terminal`, `platform`(`left`/`right`/`both`), `platformLength`(m, 기본 200), `tolerance`(정차 허용 ±m, 기본 5) |
| `signals` | `aspects`(현시 번호, 비어 있는 폐색 수 순서. 기본 `[0,1,2,3,4]` = R, YY, Y, YG, G), `spacing`(자동폐색 간격 m), `at[]`(추가 신호 위치), `departureOffset`(정차 위치에서 출발신호까지, 기본 30 m), `side`(`left`/`right`), `speeds[]`(현시별 신호 속도 km/h, 0 = 제한 없음. 기본 R 없음, YY 25, Y 45, YG 65, G 없음) |
| `safety[]` | `at`, `system`(`ats` / `tvm430` / `atp`): 그 위치부터 깔린 열차방호장치 ([safety.md](safety.md)) |
| `environment` | `sunElevation`, `sunAzimuth`(도), `ambient`, `diffuse`(0–255), `fogStart`, `fogEnd`(m) |

## 신호와 폐색

정차역(종착 제외) 앞에는 출발신호가 자동으로 생깁니다. 열차가 출발 조건(시각, 출입문)을 채우기 전까지 정지 현시입니다. 100 m보다 가까운 신호는 하나로 합칩니다. 신호기는 4등식 색등식(위에서부터 Y, G, R, Y)입니다.
