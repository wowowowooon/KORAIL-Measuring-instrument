# 보정

펌웨어 상단 상수로 보정합니다.

| 상수 | 의미 | 기본값 |
|---|---|---|
| `BASE_MM` | 두 착점 사이 거리 | 1435 |
| `L_OFFSET_MM` / `R_OFFSET_MM` | 각 레이저 0점 보정 (회전축 ~ 센서 0점) | 0 |
| `H_GAIN` / `H_OFFSET_MM` | 최종 높이 선형 보정 | 1 / 0 |

## 1. 거리 0점 (L/R 오프셋)

레이저는 회전축이 아니라 센서 기준면부터 거리를 잽니다. 그 차이만큼 `L_OFFSET_MM`, `R_OFFSET_MM` 에 더합니다.
여러 높이에서 측정해 보며 오차가 거리와 무관하게 일정한 성분을 찾아 넣습니다.

오프셋이 부족하면 낮은 높이에서 `a + b ≤ D` 가 되어 `TRI NG` 가 뜹니다.

## 2. 높이 선형 보정 (H_GAIN / H_OFFSET_MM)

1. `H_GAIN = 1`, `H_OFFSET_MM = 0` 상태로 업로드
2. 높이를 바꿔 가며(예: 800 / 850 / 900 / 950 / 1000 mm) 표시 높이와 줄자 실측값을 기록
3. CSV 로 저장
   ```csv
   measured_mm,reference_mm
   902.4,880.0
   951.8,930.0
   1001.0,980.0
   ```
4. 보정값 계산
   ```bash
   python3 tools/calibrate.py points.csv
   ```
   ```
   #define H_GAIN       1.01420f
   #define H_OFFSET_MM  -35.25f
   ...
   최대 잔차 0.07 mm (3점)
   ```
5. 출력된 두 줄을 펌웨어에 붙여 넣고 다시 업로드

측정 로그는 `tools/log_serial.py` 로 저장할 수 있습니다.

```bash
pip install pyserial
python3 tools/log_serial.py /dev/ttyUSB0 log.csv   # Windows: COM5
```

로그 형식: `ms,L_mm,R_mm,h_mm,y_mm,live` (10 Hz)
