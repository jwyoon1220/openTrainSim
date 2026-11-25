# VVVF 구동음 합성 (vvvf.json)

인버터의 변조 패턴(프리셋)만 주면 구동음을 실시간으로 합성합니다. 녹음된 모터 음원이 필요 없습니다. 실제 인버터처럼 사인파와 삼각파를 비교하는 PWM을 돌리고, 모터 선간 전압(U-V)을 소리로 냅니다.

- 열차 폴더에 `vvvf.json`을 두면 자동으로 씁니다.
- 다른 프리셋을 쓰려면 `--vvvf <파일>`이나 설정 파일의 `[sound] vvvf = <파일>`로 지정합니다. 끄려면 `none`을 줍니다.
- 소리는 운전실에서 가장 가까운 동력차에서 납니다. 기본적으로 train.dat의 모터음 표를 대신합니다(`"replace_motor": false`면 둘 다 납니다).
- 역행 중에는 소리가 나고, 타행에서는 게이트가 꺼져 조용해집니다. 제동 중에는 회생 패턴으로 나다가 `brake_cutoff_kmh` 아래에서 꺼집니다. 음량은 견인력·제동력을 따릅니다.
- HUD 통계(`showStats`)에 현재 주파수와 모드가 나옵니다(`VVVF 38.0 Hz sync 9P`).

게임 없이 들어보려면 다음 명령을 씁니다. 0 → 최고 속도 역행, 타행, 정지까지 제동을 WAV로 만들고 모드가 바뀌는 지점을 출력합니다.

```
ots_check --vvvf examples/vvvf/gto-sync.json gto.wav 100
```

예제는 `examples/vvvf/`에 있습니다(`gto-sync.json`, `igbt-async.json`).

## 형식

JSON입니다. `//`, `/* */` 주석과 끝에 붙은 쉼표를 허용합니다.

```jsonc
{
  "name": "GTO synchronous steps",
  "motor": { "poles": 4, "gear_ratio": 6.0, "wheel_diameter": 0.86, "slip_hz": 2.0 },
  "base_hz": 55,
  "power": [
    { "until_kmh": 8,  "async": 400 },
    { "until_kmh": 18, "sync": 27 },
    { "until_kmh": 25, "sync": 15 },
    { "until_kmh": 42, "sync": 3 },
    { "pulse": 1 }
  ],
  "brake": [ ... ]   // 생략하면 power와 같음
}
```

### 단계 (power / brake)

속도 순서대로 적습니다. 마지막 단계만 `until_*`를 생략할 수 있고, 그 단계가 끝까지 이어집니다.

| 필드 | 설명 |
|---|---|
| `until_kmh` / `until_hz` | 이 속도(또는 인버터 출력 주파수) 미만에서 이 단계를 씁니다. |
| `async` | 비동기 PWM. 캐리어 주파수(Hz)입니다. `[시작, 끝]`이면 이 단계 안에서 속도에 따라 올라갑니다. |
| `random` | 비동기 랜덤 변조. 캐리어 주기마다 ± 이 값(Hz) 안에서 무작위로 바뀝니다. |
| `sync` | 동기 PWM. 펄스 수 N이고, 캐리어는 N × 출력 주파수입니다(27, 15, 9, 5, 3 …). |
| `pulse` | 1펄스(`"pulse": 1`). 사각파로 출력합니다. |

### 전체 설정

| 필드 | 기본값 | 설명 |
|---|---|---|
| `motor.poles` | 4 | 모터 극수 |
| `motor.gear_ratio` | 6.0 | 기어비 |
| `motor.wheel_diameter` | 0.86 | 바퀴 지름(m) |
| `motor.slip_hz` | 1.5 | 슬립 주파수. 역행이면 더하고 제동이면 뺍니다. |
| `base_hz` | 50 | 이 주파수까지 V/f 일정(전압이 주파수에 비례)이고, 그 위는 최대 전압입니다. |
| `min_amplitude` / `max_amplitude` | 0.08 / 1.25 | 0 Hz에서의 변조율(전압 부스트), 과변조 한계 |
| `volume`, `radius` | 1, 10 | 음량, 소리 반경(m, openBVE 거리 감쇠) |
| `lowpass_hz`, `highpass_hz` | 6000, 40 | 출력 필터 |
| `brake_cutoff_kmh` | 5 | 회생제동이 끝나는 속도 |
| `replace_motor` | true | train.dat 모터음을 끕니다. |

출력 주파수(Hz)는 속도 ÷ (π × 바퀴 지름) × 기어비 × 극수/2 ± 슬립으로 계산합니다. `until_kmh`도 이 식으로 출력 주파수로 바꿔서 비교하므로, 역행과 제동 모두 적은 속도에서 정확히 단계가 바뀝니다.
