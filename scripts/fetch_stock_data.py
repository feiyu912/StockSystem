"""Fetch A-share daily bars with AkShare for StockSystem.

Usage:
  python scripts/fetch_stock_data.py 600519 000001 300750

The script writes:
  data/akshare_export_<symbol>.csv
  data/stocks.csv
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import os
import time
from pathlib import Path

KNOWN_NAMES = {
    "000001.SZ": "平安银行",
    "300750.SZ": "宁德时代",
    "600519.SH": "贵州茅台",
}


def market_suffix(symbol: str) -> str:
    if symbol.startswith(("5", "6", "9")):
        return "SH"
    return "SZ"


def output_symbol(symbol: str) -> str:
    clean = symbol.strip().upper().replace(".", "")
    if len(clean) > 6 and clean[-2:] in {"SH", "SZ"}:
        return f"{clean[:-2]}.{clean[-2:]}"
    return f"{clean}.{market_suffix(clean)}"


def disable_proxy_env() -> None:
    for name in (
        "HTTP_PROXY",
        "HTTPS_PROXY",
        "ALL_PROXY",
        "http_proxy",
        "https_proxy",
        "all_proxy",
    ):
        os.environ.pop(name, None)
    os.environ["NO_PROXY"] = "*"
    os.environ["no_proxy"] = "*"


def fetch_daily(symbol: str, start: str, end: str, retries: int):
    try:
        import akshare as ak
    except ImportError as exc:
        raise SystemExit("Please install akshare first: pip install akshare") from exc

    clean = symbol.split(".")[0]
    last_error: Exception | None = None
    for attempt in range(1, retries + 1):
        try:
            return ak.stock_zh_a_hist(
                symbol=clean,
                period="daily",
                start_date=start,
                end_date=end,
                adjust="qfq",
            )
        except Exception as exc:  # AkShare wraps network failures in requests exceptions.
            last_error = exc
            if attempt >= retries:
                break
            wait_seconds = min(2 * attempt, 8)
            print(f"{symbol}: fetch failed ({exc}); retrying in {wait_seconds}s...")
            time.sleep(wait_seconds)
    raise SystemExit(f"{symbol}: failed after {retries} attempt(s): {last_error}")


def normalize_date(value) -> int:
    if hasattr(value, "strftime"):
        return int(value.strftime("%Y%m%d"))
    text = str(value).strip()
    for fmt in ("%Y-%m-%d", "%Y/%m/%d", "%Y%m%d"):
        try:
            return int(dt.datetime.strptime(text, fmt).strftime("%Y%m%d"))
        except ValueError:
            pass
    raise ValueError(f"Unsupported date value: {value}")


def write_stock_csv(frame, symbol: str, output_dir: Path) -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    safe_symbol = symbol.replace(".", "_")
    path = output_dir / f"akshare_export_{safe_symbol}.csv"

    columns = {
        "date": "日期",
        "open": "开盘",
        "close": "收盘",
        "high": "最高",
        "low": "最低",
        "volume": "成交量",
    }
    missing = [name for name in columns.values() if name not in frame.columns]
    if missing:
        raise SystemExit(f"AkShare output is missing columns: {', '.join(missing)}")

    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["symbol", "timestamp", "open", "high", "low", "close", "volume"])
        for index, row in frame.iterrows():
            writer.writerow(
                [
                    symbol,
                    normalize_date(row[columns["date"]]),
                    row[columns["open"]],
                    row[columns["high"]],
                    row[columns["low"]],
                    row[columns["close"]],
                    int(row[columns["volume"]]),
                ]
            )
    return path


def update_stock_list(rows: list[tuple[str, str, Path]], output_dir: Path) -> None:
    list_path = output_dir / "stocks.csv"
    existing: dict[str, tuple[str, str]] = {}
    if list_path.exists():
        with list_path.open("r", newline="", encoding="utf-8") as file:
            reader = csv.DictReader(file)
            for row in reader:
                existing[row["symbol"]] = (row.get("name", ""), row.get("path", ""))

    for symbol, name, path in rows:
        existing[symbol] = (name, str(path).replace("/", "\\"))

    with list_path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["symbol", "name", "path"])
        for symbol in sorted(existing):
            name, path = existing[symbol]
            writer.writerow([symbol, name, path])


def main() -> None:
    parser = argparse.ArgumentParser(description="Fetch AkShare A-share daily bars for StockSystem.")
    parser.add_argument("symbols", nargs="+", help="A-share symbols, for example 600519 000001 300750")
    parser.add_argument("--start", default="20200101", help="Start date, format YYYYMMDD")
    parser.add_argument("--end", default="20260520", help="End date, format YYYYMMDD")
    parser.add_argument("--output-dir", default="data", help="Output directory")
    parser.add_argument("--retries", type=int, default=3, help="Network retry attempts per symbol")
    parser.add_argument("--no-proxy", action="store_true", help="Ignore HTTP(S)_PROXY environment variables")
    args = parser.parse_args()

    if args.no_proxy:
        disable_proxy_env()

    output_dir = Path(args.output_dir)
    stock_rows: list[tuple[str, str, Path]] = []
    for raw_symbol in args.symbols:
        symbol = output_symbol(raw_symbol)
        frame = fetch_daily(symbol, args.start, args.end, max(1, args.retries))
        name = KNOWN_NAMES.get(symbol, "")
        if "股票名称" in frame.columns and not frame.empty:
            name = str(frame.iloc[-1]["股票名称"])
        if "股票名称" in frame.columns and not frame.empty:
            name = str(frame.iloc[-1]["股票名称"])
        path = write_stock_csv(frame, symbol, output_dir)
        stock_rows.append((symbol, name, path))
        print(f"Wrote {path} ({len(frame)} rows)")

    update_stock_list(stock_rows, output_dir)
    print(f"Updated {output_dir / 'stocks.csv'}")


if __name__ == "__main__":
    main()
