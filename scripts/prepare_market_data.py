"""Validate and normalize StockSystem market data files.

The C++ app reads CSV files from data/ with this normalized schema:

    symbol,timestamp,open,high,low,close,volume

This script scans akshare_export_*.csv files, fixes common shape issues, writes
data/stocks.csv, and creates a compact report that is useful before backtests.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path


REQUIRED_COLUMNS = ("timestamp", "open", "high", "low", "close", "volume")
NORMALIZED_COLUMNS = ("symbol", *REQUIRED_COLUMNS)
KNOWN_NAMES = {
    "000001.SZ": "平安银行",
    "300750.SZ": "宁德时代",
    "600519.SH": "贵州茅台",
}


@dataclass
class FileReport:
    symbol: str
    path: Path
    rows_in: int
    rows_out: int
    bad_rows: int
    duplicates: int
    first_timestamp: int | None
    last_timestamp: int | None
    min_close: float | None
    max_close: float | None
    rewritten: bool


def infer_symbol(path: Path) -> str:
    stem = path.stem
    prefix = "akshare_export_"
    raw = stem[len(prefix) :] if stem.startswith(prefix) else stem
    parts = raw.rsplit("_", 1)
    if len(parts) == 2 and parts[1].upper() in {"SH", "SZ"}:
        return f"{parts[0].upper()}.{parts[1].upper()}"
    return raw.upper().replace("_", ".")


def parse_float(value: str) -> float:
    return float(value.strip())


def parse_int(value: str) -> int:
    return int(float(value.strip()))


def read_rows(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    with path.open("r", newline="", encoding="utf-8-sig") as file:
        reader = csv.DictReader(file)
        if reader.fieldnames is None:
            return [], []
        return list(reader.fieldnames), list(reader)


def normalize_file(path: Path, write: bool) -> FileReport:
    symbol = infer_symbol(path)
    headers, raw_rows = read_rows(path)
    has_symbol = "symbol" in headers
    missing = [name for name in REQUIRED_COLUMNS if name not in headers]
    if missing:
        raise ValueError(f"{path} is missing columns: {', '.join(missing)}")

    rows_by_timestamp: dict[int, dict[str, object]] = {}
    bad_rows = 0
    duplicates = 0

    for raw in raw_rows:
        try:
            timestamp = parse_int(raw["timestamp"])
            open_price = parse_float(raw["open"])
            high = parse_float(raw["high"])
            low = parse_float(raw["low"])
            close = parse_float(raw["close"])
            volume = parse_int(raw["volume"])
            row_symbol = (raw.get("symbol") or symbol).strip().upper() or symbol
            if high < max(open_price, close) or low > min(open_price, close):
                raise ValueError("invalid OHLC range")
            if volume < 0:
                raise ValueError("negative volume")
        except (KeyError, TypeError, ValueError):
            bad_rows += 1
            continue

        if timestamp in rows_by_timestamp:
            duplicates += 1
        rows_by_timestamp[timestamp] = {
            "symbol": row_symbol,
            "timestamp": timestamp,
            "open": open_price,
            "high": high,
            "low": low,
            "close": close,
            "volume": volume,
        }

    rows = [rows_by_timestamp[key] for key in sorted(rows_by_timestamp)]
    closes = [float(row["close"]) for row in rows]
    rewritten = False
    needs_rewrite = write and (bad_rows > 0 or duplicates > 0 or not has_symbol or rows)

    if needs_rewrite:
        with path.open("w", newline="", encoding="utf-8") as file:
            writer = csv.DictWriter(file, fieldnames=NORMALIZED_COLUMNS)
            writer.writeheader()
            writer.writerows(rows)
        rewritten = True

    return FileReport(
        symbol=symbol,
        path=path,
        rows_in=len(raw_rows),
        rows_out=len(rows),
        bad_rows=bad_rows,
        duplicates=duplicates,
        first_timestamp=int(rows[0]["timestamp"]) if rows else None,
        last_timestamp=int(rows[-1]["timestamp"]) if rows else None,
        min_close=min(closes) if closes else None,
        max_close=max(closes) if closes else None,
        rewritten=rewritten,
    )


def write_stock_list(reports: list[FileReport], data_dir: Path) -> Path:
    path = data_dir / "stocks.csv"
    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["symbol", "name", "path"])
        for report in sorted(reports, key=lambda item: item.symbol):
            writer.writerow([report.symbol, KNOWN_NAMES.get(report.symbol, report.symbol), str(report.path).replace("/", "\\")])
    return path


def write_report(reports: list[FileReport], data_dir: Path) -> Path:
    path = data_dir / "market_data_report.csv"
    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(
            [
                "symbol",
                "path",
                "rows_in",
                "rows_out",
                "bad_rows",
                "duplicates",
                "first_timestamp",
                "last_timestamp",
                "min_close",
                "max_close",
                "rewritten",
            ]
        )
        for report in sorted(reports, key=lambda item: item.symbol):
            writer.writerow(
                [
                    report.symbol,
                    report.path,
                    report.rows_in,
                    report.rows_out,
                    report.bad_rows,
                    report.duplicates,
                    report.first_timestamp or "",
                    report.last_timestamp or "",
                    f"{report.min_close:.6f}" if report.min_close is not None else "",
                    f"{report.max_close:.6f}" if report.max_close is not None else "",
                    int(report.rewritten),
                ]
            )
    return path


def main() -> None:
    parser = argparse.ArgumentParser(description="Validate and normalize StockSystem CSV market data.")
    parser.add_argument("--data-dir", default="data", help="Directory containing akshare_export_*.csv files.")
    parser.add_argument("--write", action="store_true", help="Rewrite normalized CSV files and generated indexes.")
    args = parser.parse_args()

    data_dir = Path(args.data_dir)
    files = sorted(data_dir.glob("akshare_export_*.csv"))
    if not files:
        raise SystemExit(f"No akshare_export_*.csv files found under {data_dir}")

    reports = [normalize_file(path, args.write) for path in files]
    if args.write:
        stock_list = write_stock_list(reports, data_dir)
        report_path = write_report(reports, data_dir)
        print(f"Wrote {stock_list}")
        print(f"Wrote {report_path}")

    for report in reports:
        status = "rewritten" if report.rewritten else "checked"
        print(
            f"{report.symbol}: {status}, rows={report.rows_out}, "
            f"bad={report.bad_rows}, duplicates={report.duplicates}, "
            f"close=[{report.min_close}, {report.max_close}]"
        )


if __name__ == "__main__":
    main()
