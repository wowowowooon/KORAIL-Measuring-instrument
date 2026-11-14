#!/usr/bin/env python3
"""장치 시리얼 CSV를 파일로 저장 (Ctrl+C로 종료).

사용: python3 tools/log_serial.py <포트> [출력.csv]
예:   python3 tools/log_serial.py /dev/ttyUSB0 log.csv
      python3 tools/log_serial.py COM5 log.csv
필요: pip install pyserial
"""
import sys

import serial


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    port = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "log.csv"
    with serial.Serial(port, 115200, timeout=1) as ser, open(out, "w") as f:
        print(f"{port} → {out}  (Ctrl+C 종료)")
        try:
            while True:
                line = ser.readline().decode("utf-8", "replace").strip()
                if not line:
                    continue
                print(line)
                if not line.startswith("#"):  # 상태 메시지는 화면에만
                    f.write(line + "\n")
                    f.flush()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
