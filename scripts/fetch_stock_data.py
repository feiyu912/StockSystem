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
from pathlib import Path


def market_suffix(symbol: str) -> str:
    if symbol.startswith(("5", "6", "9")):
        return "SH"
    return "SZ"


def output_symbol(symbol: str) -> str:
    clean = symbol.strip().upper().replace(".", "")
    if len(clean) > 6 and clean[-2:] in {"SH", "SZ"}:
        return f"{clean[:-2]}.{clean[-2:]}"
    return f"{clean}.{market_suffix(clean)}"


def fetch_daily(symbol: str, start: str, end: str):
    try:
        import akshare as ak
    except ImportError as exc:
        raise SystemExit("Please install akshare first: pip install akshare") from exc

    clean = symbol.split(".")[0]
    return ak.stock_zh_a_hist(
        symbol=clean,
        period="daily",
        start_date=start,
        end_date=end,
        adjust="qfq",
    )


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
                    index + 1,
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
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    stock_rows: list[tuple[str, str, Path]] = []
    for raw_symbol in args.symbols:
        symbol = output_symbol(raw_symbol)
        frame = fetch_daily(symbol, args.start, args.end)
        name = ""
        if "股票名称" in frame.columns and not frame.empty:
            name = str(frame.iloc[-1]["股票名称"])
        path = write_stock_csv(frame, symbol, output_dir)
        stock_rows.append((symbol, name, path))
        print(f"Wrote {path} ({len(frame)} rows)")

    update_stock_list(stock_rows, output_dir)
    print(f"Updated {output_dir / 'stocks.csv'}")


if __name__ == "__main__":
    main()
