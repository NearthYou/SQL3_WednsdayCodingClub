#!/usr/bin/env python3
import argparse
import concurrent.futures
import http.client
import json
import math
import os
import random
import statistics
import time
from datetime import datetime, timezone
from urllib.parse import urlparse


MIXES = {
    "read-heavy": (80, 10, 8, 2),
    "balanced": (55, 20, 20, 5),
    "write-heavy": (30, 35, 25, 10),
    "select-only": (100, 0, 0, 0),
}


def choose_operation(rng, mix_name):
    select, insert, update, delete = MIXES[mix_name]
    pick = rng.randint(1, 100)
    if pick <= select:
        return "SELECT"
    if pick <= select + insert:
        return "INSERT"
    if pick <= select + insert + update:
        return "UPDATE"
    return "DELETE"


def build_sql(op, index, rng):
    if op == "SELECT":
        key = rng.choice([1, 2, 3])
        return f"SELECT * FROM case_basic_users WHERE id = {key};"
    if op == "INSERT":
        key = 900000 + index
        return (
            "INSERT INTO case_basic_users VALUES "
            f"({key},'load{key}@test.com','010-{key % 10000:04d}','pass{key}','Load{key}');"
        )
    if op == "UPDATE":
        return f"UPDATE case_basic_users SET name = 'LoadUpdated{index}' WHERE id = 1;"
    return f"DELETE FROM case_basic_users WHERE id = {950000 + index};"


def percentile(values, pct):
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = math.ceil((pct / 100.0) * len(ordered)) - 1
    idx = max(0, min(idx, len(ordered) - 1))
    return ordered[idx]


def send_request(url, sql, timeout_sec):
    parsed = urlparse(url)
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    path = parsed.path or "/query"
    start = time.perf_counter()
    status = 0
    body = ""
    error = None
    try:
        if parsed.scheme == "https":
            conn = http.client.HTTPSConnection(host, port, timeout=timeout_sec)
        else:
            conn = http.client.HTTPConnection(host, port, timeout=timeout_sec)
        conn.request("POST", path, body=sql.encode("utf-8"), headers={"Content-Type": "text/plain"})
        response = conn.getresponse()
        status = response.status
        body = response.read().decode("utf-8", errors="replace")
        conn.close()
        if '"status":' not in body:
            error = "non_json_response"
    except Exception as exc:
        error = type(exc).__name__
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    return {
        "status": status,
        "elapsed_ms": elapsed_ms,
        "ok": status == 200 and error is None,
        "error": error,
    }


def write_markdown(report_path, data):
    md_path = os.path.splitext(report_path)[0] + ".md"
    lines = [
        "# API Load Test Report",
        "",
        f"- profile: `{data['profile']}`",
        f"- mix: `{data['mix']}`",
        f"- seed: `{data['seed']}`",
        f"- url: `{data['url']}`",
        f"- requests: `{data['requests']}`",
        f"- concurrency: `{data['concurrency']}`",
        f"- success: `{data['success']}`",
        f"- errors: `{data['errors']}`",
        f"- requests/sec: `{data['requests_per_sec']:.2f}`",
        "",
        "## Latency",
        "",
        f"- p50_ms: `{data['latency_ms']['p50']:.2f}`",
        f"- p95_ms: `{data['latency_ms']['p95']:.2f}`",
        f"- p99_ms: `{data['latency_ms']['p99']:.2f}`",
        f"- max_ms: `{data['latency_ms']['max']:.2f}`",
        "",
        "## Status Counts",
        "",
    ]
    for status, count in sorted(data["status_counts"].items()):
        lines.append(f"- `{status}`: `{count}`")
    lines.extend(["", "## Error Counts", ""])
    if data["error_counts"]:
        for err, count in sorted(data["error_counts"].items()):
            lines.append(f"- `{err}`: `{count}`")
    else:
        lines.append("- none")
    with open(md_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description="Run API load test and write JSON/Markdown reports.")
    parser.add_argument("--url", default="http://127.0.0.1:8080/query")
    parser.add_argument("--concurrency", type=int, default=50)
    parser.add_argument("--requests", type=int, default=1000)
    parser.add_argument("--mix", choices=sorted(MIXES.keys()), default="read-heavy")
    parser.add_argument("--seed", type=int, default=20260415)
    parser.add_argument("--timeout-sec", type=float, default=5.0)
    parser.add_argument("--profile", default="api-regression")
    parser.add_argument("--report", default="artifacts/api-load/report.json")
    args = parser.parse_args()

    rng = random.Random(args.seed)
    jobs = []
    for i in range(args.requests):
        op = choose_operation(rng, args.mix)
        jobs.append(build_sql(op, i + 1, rng))

    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as executor:
        futures = [executor.submit(send_request, args.url, sql, args.timeout_sec) for sql in jobs]
        results = [future.result() for future in concurrent.futures.as_completed(futures)]
    elapsed = time.perf_counter() - started

    latencies = [item["elapsed_ms"] for item in results]
    status_counts = {}
    error_counts = {}
    success = 0
    for item in results:
        status_counts[str(item["status"])] = status_counts.get(str(item["status"]), 0) + 1
        if item["ok"]:
            success += 1
        if item["error"]:
            error_counts[item["error"]] = error_counts.get(item["error"], 0) + 1

    report = {
        "profile": args.profile,
        "mix": args.mix,
        "seed": args.seed,
        "url": args.url,
        "concurrency": args.concurrency,
        "requests": args.requests,
        "success": success,
        "errors": args.requests - success,
        "total_elapsed_seconds": elapsed,
        "requests_per_sec": args.requests / elapsed if elapsed > 0 else 0.0,
        "latency_ms": {
            "p50": percentile(latencies, 50),
            "p95": percentile(latencies, 95),
            "p99": percentile(latencies, 99),
            "max": max(latencies) if latencies else 0.0,
            "mean": statistics.mean(latencies) if latencies else 0.0,
        },
        "status_counts": status_counts,
        "error_counts": error_counts,
        "generated_at": datetime.now(timezone.utc).isoformat(),
    }

    os.makedirs(os.path.dirname(args.report), exist_ok=True)
    with open(args.report, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
        f.write("\n")
    write_markdown(args.report, report)
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
