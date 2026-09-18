#!/usr/bin/env python3
"""Plot recoil dump CSV files into a self-contained HTML report.

No third-party dependencies -- pure standard library. Emits one HTML file with
inline SVG, so it opens in any browser with no server and no network access.

Why this exists
---------------
The recoil system's acceptance process (see Docs/RecoilDevelopmentPlan.md, P5)
requires CSV curves as the standard evidence for tuning, and P7 requires
overlaying two curves to compare tunings. This script is the "chart tool" side
of that workflow.

The CSV comes from the in-game command:

    Lyra.Recoil.Dump          -> Saved/RecoilDump_<timestamp>.csv

Columns: ShotIndex,VerticalKick,HorizontalKick,AccumulatedPitch,AccumulatedYaw,TimeSinceFire[,RollShake]
(column order is defined by the plan and locked by Lyra.Recoil.Dump.HeaderSchema)

Usage
-----
    # one curve
    python plot-recoil-csv.py Saved/RecoilDump_20260917_204500_123.csv

    # overlay two tunings (each file becomes its own series)
    python plot-recoil-csv.py before.csv after.csv -o compare.html

    # glob everything in Saved/
    python plot-recoil-csv.py "Saved/RecoilDump_*.csv" -o all.html
"""

import argparse
import csv
import glob
import html
import os
import sys
from datetime import datetime

# Palette: distinguishable in print and on screen.
SERIES_COLORS = [
    "#c0392b", "#2471a3", "#1e8449", "#b7950b",
    "#7d3c98", "#d35400", "#138d75", "#566573",
]

CHART_WIDTH = 1080
CHART_HEIGHT = 420
PADDING_LEFT = 78
PADDING_RIGHT = 190
PADDING_TOP = 46
PADDING_BOTTOM = 56

REQUIRED_COLUMNS = [
    "ShotIndex",
    "VerticalKick",
    "HorizontalKick",
    "AccumulatedPitch",
    "AccumulatedYaw",
    "TimeSinceFire",
]

# P8 追加的列。**可选**：旧 CSV（6 列）仍然能画，只是不出 Roll 曲线。
# 语义提醒：RollShake 是"开火瞬间"的解析解采样，不是本发累计量 ——
# 所以它的纵轴量级远小于 AccumulatedPitch，别放进同一张图直接比大小。
OPTIONAL_COLUMNS = [
    "RollShake",
]


def load_csv(path):
    """Return a dict with the parsed columns, or raise ValueError."""
    with open(path, "r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        fieldnames = reader.fieldnames or []

        missing = [c for c in REQUIRED_COLUMNS if c not in fieldnames]
        if missing:
            raise ValueError(
                "missing column(s) %s; header was %s" % (", ".join(missing), fieldnames)
            )

        has_roll = all(c in fieldnames for c in OPTIONAL_COLUMNS)

        rows = []
        for record in reader:
            row = {
                "shot": int(float(record["ShotIndex"])),
                "vertical_kick": float(record["VerticalKick"]),
                "horizontal_kick": float(record["HorizontalKick"]),
                "accumulated_pitch": float(record["AccumulatedPitch"]),
                "accumulated_yaw": float(record["AccumulatedYaw"]),
                "time_since_fire": float(record["TimeSinceFire"]),
            }
            if has_roll:
                row["roll_shake"] = float(record["RollShake"])
            rows.append(row)

    if not rows:
        raise ValueError("no data rows")

    return {"path": path, "rows": rows, "has_roll": has_roll}


def nice_bounds(values):
    """Symmetric-ish bounds around the data with a little headroom."""
    if not values:
        return -1.0, 1.0

    lo = min(values)
    hi = max(values)

    if lo == hi:
        return lo - 1.0, hi + 1.0

    span = hi - lo
    pad = span * 0.12
    return lo - pad, hi + pad


def build_axes(series_list, value_getter):
    """Shared x (shot index) and y (value) ranges across all series."""
    xs, ys = [], []
    for series in series_list:
        for row in series["rows"]:
            xs.append(row["shot"])
            ys.append(value_getter(row))

    x_min = min(xs) if xs else 0
    x_max = max(xs) if xs else 1
    if x_min == x_max:
        x_max = x_min + 1

    y_min, y_max = nice_bounds(ys)
    return x_min, x_max, y_min, y_max


def make_mapper(x_min, x_max, y_min, y_max):
    plot_w = CHART_WIDTH - PADDING_LEFT - PADDING_RIGHT
    plot_h = CHART_HEIGHT - PADDING_TOP - PADDING_BOTTOM

    def sx(x):
        return PADDING_LEFT + (x - x_min) / (x_max - x_min) * plot_w

    def sy(y):
        return PADDING_TOP + (y_max - y) / (y_max - y_min) * plot_h

    return sx, sy


def svg_header(title, y_label):
    parts = []
    parts.append(
        '<svg viewBox="0 0 %d %d" width="100%%" role="img" aria-label="%s">'
        % (CHART_WIDTH, CHART_HEIGHT, html.escape(title))
    )
    parts.append(
        '<text x="%d" y="26" font-family="Segoe UI, Arial, sans-serif" '
        'font-size="15" font-weight="600" fill="#1b2631">%s</text>'
        % (PADDING_LEFT, html.escape(title))
    )
    parts.append(
        '<text x="16" y="%d" font-family="Segoe UI, Arial, sans-serif" '
        'font-size="11" fill="#5d6d7e" transform="rotate(-90 16 %d)" '
        'text-anchor="middle">%s</text>'
        % (CHART_HEIGHT // 2, CHART_HEIGHT // 2, html.escape(y_label))
    )
    return parts


def svg_frame(sx, sy, x_min, x_max, y_min, y_max, y_ticks=5):
    parts = []
    plot_right = CHART_WIDTH - PADDING_RIGHT
    plot_bottom = CHART_HEIGHT - PADDING_BOTTOM

    # grid + y ticks
    for i in range(y_ticks + 1):
        value = y_min + (y_max - y_min) * i / y_ticks
        y = sy(value)
        parts.append(
            '<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#e5e8ea" stroke-width="1"/>'
            % (PADDING_LEFT, y, plot_right, y)
        )
        parts.append(
            '<text x="%d" y="%.1f" font-family="Consolas, monospace" font-size="10" '
            'fill="#5d6d7e" text-anchor="end">%.3f</text>'
            % (PADDING_LEFT - 8, y + 3.5, value)
        )

    # x ticks (integer shot index)
    span = x_max - x_min
    step = 1 if span <= 20 else max(1, span // 12)
    shot = x_min
    while shot <= x_max:
        x = sx(shot)
        parts.append(
            '<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="#e5e8ea" stroke-width="1"/>'
            % (x, PADDING_TOP, x, plot_bottom)
        )
        parts.append(
            '<text x="%.1f" y="%d" font-family="Consolas, monospace" font-size="10" '
            'fill="#5d6d7e" text-anchor="middle">%d</text>'
            % (x, plot_bottom + 16, shot)
        )
        shot += step

    parts.append(
        '<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#839192" stroke-width="1.5"/>'
        % (PADDING_LEFT, plot_bottom, plot_right, plot_bottom)
    )
    parts.append(
        '<line x1="%d" y1="%d" x2="%d" y2="%.1f" stroke="#839192" stroke-width="1.5"/>'
        % (PADDING_LEFT, PADDING_TOP, PADDING_LEFT, plot_bottom)
    )

    # zero line, if visible
    if y_min < 0.0 < y_max:
        y0 = sy(0.0)
        parts.append(
            '<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#aab7b8" '
            'stroke-width="1" stroke-dasharray="4 3"/>' % (PADDING_LEFT, y0, plot_right, y0)
        )

    parts.append(
        '<text x="%d" y="%d" font-family="Segoe UI, Arial, sans-serif" font-size="11" '
        'fill="#5d6d7e" text-anchor="middle">Shot index</text>'
        % ((PADDING_LEFT + plot_right) // 2, CHART_HEIGHT - 18)
    )
    return parts


def render_chart(series_list, value_getter, title, y_label, legend_labels):
    x_min, x_max, y_min, y_max = build_axes(series_list, value_getter)
    sx, sy = make_mapper(x_min, x_max, y_min, y_max)

    parts = svg_header(title, y_label)
    parts.extend(svg_frame(sx, sy, x_min, x_max, y_min, y_max))

    for index, series in enumerate(series_list):
        color = SERIES_COLORS[index % len(SERIES_COLORS)]
        points = [
            (sx(row["shot"]), sy(value_getter(row)))
            for row in series["rows"]
        ]

        path_d = " ".join(
            ("M" if i == 0 else "L") + "%.1f %.1f" % p for i, p in enumerate(points)
        )
        parts.append(
            '<path d="%s" fill="none" stroke="%s" stroke-width="2.2" '
            'stroke-linejoin="round" stroke-linecap="round"/>' % (path_d, color)
        )

        for px, py in points:
            parts.append(
                '<circle cx="%.1f" cy="%.1f" r="2.6" fill="%s"/>' % (px, py, color)
            )

    # legend
    legend_x = CHART_WIDTH - PADDING_RIGHT + 16
    legend_y = PADDING_TOP + 4
    for index, label in enumerate(legend_labels):
        color = SERIES_COLORS[index % len(SERIES_COLORS)]
        parts.append(
            '<rect x="%d" y="%d" width="11" height="11" rx="2" fill="%s"/>'
            % (legend_x, legend_y + index * 20, color)
        )
        parts.append(
            '<text x="%d" y="%d" font-family="Consolas, monospace" font-size="10.5" '
            'fill="#1b2631">%s</text>'
            % (legend_x + 17, legend_y + 10 + index * 20, html.escape(label))
        )

    parts.append("</svg>")
    return "\n".join(parts)


def render_table(series):
    rows = series["rows"]
    head = "".join("<th>%s</th>" % html.escape(c) for c in REQUIRED_COLUMNS)
    body = []
    for row in rows:
        body.append(
            "<tr><td>%d</td><td>%.4f</td><td>%.4f</td><td>%.4f</td><td>%.4f</td><td>%.4f</td></tr>"
            % (
                row["shot"],
                row["vertical_kick"],
                row["horizontal_kick"],
                row["accumulated_pitch"],
                row["accumulated_yaw"],
                row["time_since_fire"],
            )
        )
    return (
        '<table><thead><tr>%s</tr></thead><tbody>%s</tbody></table>'
        % (head, "".join(body))
    )


def summarize(series):
    rows = series["rows"]
    last = rows[-1]
    times = [r["time_since_fire"] for r in rows]
    return {
        "shots": len(rows),
        "peak_pitch": max(r["accumulated_pitch"] for r in rows),
        "peak_yaw": max(r["accumulated_yaw"] for r in rows),
        "final_pitch": last["accumulated_pitch"],
        "final_yaw": last["accumulated_yaw"],
        "total_time": sum(times),
    }


def build_html(loaded, output_path):
    labels = [os.path.basename(s["path"]) for s in loaded]

    pitch_chart = render_chart(
        loaded, lambda r: r["accumulated_pitch"],
        "AccumulatedPitch over shot index", "degrees (up = +)", labels,
    )
    yaw_chart = render_chart(
        loaded, lambda r: r["accumulated_yaw"],
        "AccumulatedYaw over shot index", "degrees (right = +)", labels,
    )
    kick_chart = render_chart(
        loaded, lambda r: r["vertical_kick"],
        "Per-shot VerticalKick", "degrees", labels,
    )

    summary_rows = []
    for series, label in zip(loaded, labels):
        stats = summarize(series)
        summary_rows.append(
            "<tr><td class='mono'>%s</td><td>%d</td><td>%.4f</td><td>%.4f</td>"
            "<td>%.4f</td><td>%.4f</td><td>%.4f</td></tr>"
            % (
                html.escape(label), stats["shots"], stats["peak_pitch"], stats["peak_yaw"],
                stats["final_pitch"], stats["final_yaw"], stats["total_time"],
            )
        )

    tables = "\n".join(
        "<h3 class='mono'>%s</h3>%s" % (html.escape(label), render_table(series))
        for series, label in zip(loaded, labels)
    )

    generated = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    plotter = os.path.basename(__file__)

    return """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>Recoil Dump Report</title>
<style>
  :root {{ color-scheme: light; }}
  * {{ box-sizing: border-box; }}
  body {{
    margin: 0; padding: 32px 40px 64px;
    background: #f7f9fa; color: #1b2631;
    font-family: "Segoe UI", "Microsoft YaHei", Arial, sans-serif;
    font-size: 14px; line-height: 1.6;
  }}
  h1 {{ font-size: 22px; margin: 0 0 4px; }}
  h2 {{ font-size: 16px; margin: 36px 0 12px; padding-bottom: 6px;
        border-bottom: 1px solid #dce1e4; }}
  h3 {{ font-size: 13px; margin: 24px 0 8px; }}
  .sub {{ color: #5d6d7e; font-size: 12.5px; margin-bottom: 24px; }}
  .card {{ background: #ffffff; border: 1px solid #e1e6e9; border-radius: 8px;
           padding: 18px 20px 10px; margin-bottom: 20px; }}
  .mono {{ font-family: Consolas, "Courier New", monospace; }}
  table {{ border-collapse: collapse; width: 100%; font-size: 12.5px; }}
  th, td {{ border: 1px solid #e1e6e9; padding: 5px 10px; text-align: right; }}
  th {{ background: #eef2f4; font-weight: 600; text-align: right; }}
  td:first-child, th:first-child {{ text-align: left; }}
  tbody tr:nth-child(even) {{ background: #fafcfc; }}
  .meta {{ font-size: 12.5px; color: #5d6d7e; }}
  code {{ background: #eef2f4; padding: 1px 5px; border-radius: 3px;
          font-family: Consolas, monospace; font-size: 12px; }}
</style>
</head>
<body>
<h1>后坐力 CSV 曲线报告</h1>
<div class="sub">
  数据源：游戏内 <code>Lyra.Recoil.Dump</code> 导出的 CSV（列定义见
  <code>Lyra.Recoil.Dump.HeaderSchema</code>）。本页由
  <span class="mono">{plotter}</span> 生成（纯标准库，无外部依赖）。
</div>

<h2>总览</h2>
<div class="card">
<table>
  <thead><tr>
    <th>CSV</th><th>发数</th><th>峰值 Pitch</th><th>峰值 Yaw</th>
    <th>末发 Pitch</th><th>末发 Yaw</th><th>累计射击时长 (s)</th>
  </tr></thead>
  <tbody>{summary_rows}</tbody>
</table>
</div>

<h2>曲线</h2>
<div class="card">{pitch_chart}</div>
<div class="card">{yaw_chart}</div>
<div class="card">{kick_chart}</div>

<h2>逐发数据</h2>
<div class="card">{tables}</div>

<div class="meta" style="margin-top:28px">
  生成时间：{generated}
</div>
</body>
</html>
""".format(
        plotter=html.escape(plotter),
        summary_rows="".join(summary_rows),
        pitch_chart=pitch_chart,
        yaw_chart=yaw_chart,
        kick_chart=kick_chart,
        tables=tables,
        generated=generated,
    )


def expand_inputs(patterns):
    paths = []
    for pattern in patterns:
        if any(ch in pattern for ch in "*?["):
            matched = sorted(glob.glob(pattern))
            if not matched:
                print("warning: no file matched %s" % pattern, file=sys.stderr)
            paths.extend(matched)
        else:
            paths.append(pattern)
    return paths


def main():
    parser = argparse.ArgumentParser(
        description="Plot recoil dump CSV files into a self-contained HTML report."
    )
    parser.add_argument("csv", nargs="+", help="CSV path(s) or glob pattern(s)")
    parser.add_argument("-o", "--output", default=None,
                        help="output HTML path (default: RecoilCurves.html next to the first CSV)")
    args = parser.parse_args()

    paths = expand_inputs(args.csv)
    if not paths:
        print("error: no input files", file=sys.stderr)
        return 1

    loaded = []
    for path in paths:
        try:
            loaded.append(load_csv(path))
            print("loaded %s" % path)
        except (OSError, ValueError) as exc:
            print("error: %s: %s" % (path, exc), file=sys.stderr)

    if not loaded:
        print("error: nothing to plot", file=sys.stderr)
        return 1

    output = args.output
    if output is None:
        output = os.path.join(os.path.dirname(os.path.abspath(paths[0])), "RecoilCurves.html")

    with open(output, "w", encoding="utf-8") as handle:
        handle.write(build_html(loaded, output))

    print("wrote %s (%d series)" % (output, len(loaded)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
