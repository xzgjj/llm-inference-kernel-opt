"""纯标准库 PNG 折线图：不引 matplotlib/numpy，归档离线也能出图（N-07）。"""

from __future__ import annotations

import csv
import struct
import zlib
from pathlib import Path

WIDTH = 800
HEIGHT = 480
COLORS = [(31, 119, 180), (214, 39, 40)]
FONT = {
    " ": ["00000"] * 7,
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "B": ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    "C": ["01111", "10000", "10000", "10000", "10000", "10000", "01111"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "F": ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    "G": ["01111", "10000", "10000", "10111", "10001", "10001", "01111"],
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "I": ["11111", "00100", "00100", "00100", "00100", "00100", "11111"],
    "J": ["00111", "00010", "00010", "00010", "00010", "10010", "01100"],
    "K": ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "N": ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "Q": ["01110", "10001", "10001", "10001", "10101", "10010", "01101"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "U": ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    "V": ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    "W": ["10001", "10001", "10001", "10101", "10101", "11011", "10001"],
    "X": ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "Z": ["11111", "00001", "00010", "00100", "01000", "10000", "11111"],
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00110", "01000", "10000", "11111"],
    "3": ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    ".": ["00000", "00000", "00000", "00000", "00000", "00110", "00110"],
    "-": ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
}


class Canvas:
    def __init__(self) -> None:
        self.data = bytearray([255] * (WIDTH * HEIGHT * 3))

    def pixel(self, x: int, y: int, color: tuple[int, int, int]) -> None:
        if 0 <= x < WIDTH and 0 <= y < HEIGHT:
            offset = (y * WIDTH + x) * 3
            self.data[offset:offset + 3] = bytes(color)

    def line(self, x0: int, y0: int, x1: int, y1: int,
             color: tuple[int, int, int]) -> None:
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        error = dx + dy
        while True:
            self.pixel(x0, y0, color)
            if x0 == x1 and y0 == y1:
                return
            doubled = 2 * error
            if doubled >= dy:
                error += dy
                x0 += sx
            if doubled <= dx:
                error += dx
                y0 += sy

    def text(self, x: int, y: int, text: str, color: tuple[int, int, int],
             scale: int = 2) -> None:
        for index, char in enumerate(text.upper()):
            glyph = FONT.get(char, FONT[" "])
            for row, bits in enumerate(glyph):
                for column, bit in enumerate(bits):
                    if bit == "1":
                        for dy in range(scale):
                            for dx in range(scale):
                                self.pixel(x + (index * 6 + column) * scale + dx,
                                           y + row * scale + dy, color)

    def png(self, path: Path) -> None:
        raw = b"".join(b"\x00" + bytes(self.data[y * WIDTH * 3:(y + 1) * WIDTH * 3])
                       for y in range(HEIGHT))
        path.write_bytes(
            b"\x89PNG\r\n\x1a\n"
            + _chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0))
            + _chunk(b"IDAT", zlib.compress(raw, 6))
            + _chunk(b"IEND", b"")
        )


def _chunk(tag: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


def _load(path: Path) -> list[dict]:
    with path.open(encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def _draw_chart(rows: list[dict], field: str, title: str, path: Path) -> None:
    canvas = Canvas()
    left, right, top, bottom = 70, WIDTH - 40, 70, HEIGHT - 60
    canvas.text(left, 20, title, (20, 20, 20), 3)
    canvas.line(left, top, left, bottom, (90, 90, 90))
    canvas.line(left, bottom, right, bottom, (90, 90, 90))

    series = sorted({row["prompt_tier"] for row in rows})
    values: list[float] = []
    for row in rows:
        raw = row.get(field, "")
        if raw not in ("", "None"):
            values.append(float(raw))
    low, high = (0.0, 1.0) if not values else (min(values), max(values))
    if high == low:
        high = low + 1.0
    pad = (high - low) * 0.1
    low -= pad
    high += pad

    xs = sorted({int(row["concurrency_p"]) for row in rows})
    log_x = {value: (value.bit_length() - 1) for value in xs}
    max_log = max(log_x.values()) or 1

    def x_at(concurrency: int) -> int:
        return left + int((right - left) * log_x[concurrency] / max_log)

    def y_at(value: float) -> int:
        return bottom - int((bottom - top) * (value - low) / (high - low))

    for index in range(5):
        y = top + int((bottom - top) * index / 4)
        canvas.line(left, y, right, y, (225, 225, 225))
        canvas.text(8, y - 7, f"{high - (high - low) * index / 4:.1f}", (80, 80, 80), 2)

    for concurrency in xs:
        x = x_at(concurrency)
        canvas.text(x - 5, bottom + 8, str(concurrency), (80, 80, 80), 2)

    for series_index, tier in enumerate(series):
        points = []
        for row in rows:
            if row["prompt_tier"] != tier:
                continue
            raw = row.get(field, "")
            if raw in ("", "None"):
                continue
            points.append((x_at(int(row["concurrency_p"])), y_at(float(raw))))
        color = COLORS[series_index % len(COLORS)]
        for first, second in zip(points, points[1:]):
            canvas.line(first[0], first[1], second[0], second[1], color)
        for x, y in points:
            canvas.line(x - 2, y, x + 2, y, color)
            canvas.line(x, y - 2, x, y + 2, color)
        canvas.text(right - 150, top + 10 + series_index * 22, tier.upper(), color, 2)
    path.parent.mkdir(parents=True, exist_ok=True)
    canvas.png(path)


def plot_directory(directory: Path) -> int:
    rows = _load(directory / "matrix_report.csv")
    if not rows:
        print(f"[plot] {directory}/matrix_report.csv 为空")
        return 1
    plots = [
        ("ttft_p50_ms", "TTFT P50 MS", "ttft_p50.png"),
        ("itl_p99_ms", "ITL P99 MS", "itl_p99.png"),
        ("total_tok_s", "TOTAL TOKENS PER SEC", "total_tok_s.png"),
        ("sched_cpu_ratio", "SCHED CPU RATIO", "sched_cpu_ratio.png"),
    ]
    out_dir = directory / "plots"
    for field, title, name in plots:
        _draw_chart(rows, field, title, out_dir / name)
    print(f"[plot] {len(plots)} 张 PNG -> {out_dir}")
    return 0
