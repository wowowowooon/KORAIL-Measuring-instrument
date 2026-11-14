#!/usr/bin/env python3
"""높이 선형 보정값(H_GAIN, H_OFFSET_MM) 계산.

입력 CSV: 각 행에 장치가 표시한 높이와 줄자로 잰 실제 높이.
    measured_mm,reference_mm
    902.4,880.0
    951.0,930.0
    ...

보정 전 값(H_GAIN=1, H_OFFSET_MM=0 상태에서 측정)을 넣으면
reference ≈ gain * measured + offset 을 최소제곱으로 구해 펌웨어에 넣을 값을 출력한다.

사용: python3 tools/calibrate.py points.csv
"""
import csv
import sys


def fit(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return 1.0, my - mx  # 한 높이뿐이면 절편만
    gain = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    return gain, my - gain * mx


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    xs, ys = [], []
    with open(sys.argv[1], newline="") as f:
        for row in csv.DictReader(f):
            xs.append(float(row["measured_mm"]))
            ys.append(float(row["reference_mm"]))
    if not xs:
        sys.exit("데이터가 없습니다")

    gain, offset = fit(xs, ys)
    print(f"#define H_GAIN       {gain:.5f}f")
    print(f"#define H_OFFSET_MM  {offset:.2f}f")
    print()
    print(" 측정      실제     보정후    잔차")
    worst = 0.0
    for x, y in zip(xs, ys):
        c = gain * x + offset
        worst = max(worst, abs(c - y))
        print(f"{x:7.1f}  {y:7.1f}  {c:7.1f}  {c - y:+6.2f}")
    print(f"\n최대 잔차 {worst:.2f} mm ({len(xs)}점)")


if __name__ == "__main__":
    main()
