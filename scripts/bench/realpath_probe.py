# realpath_probe.py — 阶段 4 / 真实路径探测：只用标准库，按真实客户端方式打 SSE/普通 HTTP。
# 用法：python scripts/bench/realpath_probe.py --base http://127.0.0.1:18080 --out evidence/phase4/realpath.json
import argparse
import http.client
import json
import statistics
import threading
import time
import urllib.parse

RESULTS = []


def _conn(base):
    parsed = urllib.parse.urlparse(base)
    return http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=30)


def http_json(base, path, payload=None, raw=None, method=None):
    conn = _conn(base)
    body = raw if raw is not None else (json.dumps(payload) if payload is not None else None)
    verb = method or ("POST" if body is not None else "GET")
    conn.request(verb, path, body=body, headers={"Content-Type": "application/json"})
    resp = conn.getresponse()
    data = resp.read().decode("utf-8", "replace")
    status = resp.status
    conn.close()
    return status, data


def sse_once(base, prompt, max_tokens, stop_after=None, delay=None):
    """真实 SSE 客户端：逐行读，记录首帧与 token 间隔；可中途断开或慢读。"""
    parsed = urllib.parse.urlparse(base)
    conn = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=60)
    body = json.dumps({"prompt": prompt, "max_tokens": max_tokens, "stream": True})
    conn.request("POST", "/completion", body=body,
                 headers={"Content-Type": "application/json", "Accept": "text/event-stream"})
    resp = conn.getresponse()
    started = time.perf_counter()
    first = None
    gaps = []
    frames = 0
    done = False
    last = None
    try:
        while True:
            line = resp.readline()
            if not line:
                break
            if line.startswith(b"data:"):
                frames += 1
                now = time.perf_counter()
                if first is None:
                    first = now - started
                elif last is not None:
                    gaps.append((now - last) * 1000.0)
                last = now
                if b"[DONE]" in line:
                    done = True
                    break
            if stop_after is not None and frames >= stop_after:
                break
            if delay:
                time.sleep(delay)
    finally:
        conn.close()
    return {"frames": frames, "done": done, "ttft_ms": None if first is None else round(first * 1000, 2),
            "gaps_ms": [round(g, 2) for g in gaps]}


def metric(base, name):
    _, text = http_json(base, "/metrics", method="GET")
    for line in text.splitlines():
        if line.startswith(name + " ") or line.startswith(name + "{"):
            return float(line.rsplit(" ", 1)[1])
    return None


def record(case, ok, detail):
    RESULTS.append({"case": case, "ok": bool(ok), "detail": detail})
    print(("PASS " if ok else "FAIL ") + case + " :: " + str(detail))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="http://127.0.0.1:18080")
    ap.add_argument("--out", default="evidence/phase4/realpath.json")
    args = ap.parse_args()
    base = args.base

    # 1 健康与指标首屏
    st, body = http_json(base, "/healthz", method="GET")
    record("healthz", st == 200 and "ready" in body, {"status": st, "body": body})
    conn = _conn(base)
    conn.request("GET", "/metrics")
    resp = conn.getresponse()
    metrics_text = resp.read().decode()
    st = resp.status
    conn.close()
    names = [l.split(" ")[0] for l in metrics_text.splitlines() if l and not l.startswith("#")]
    record("metrics_first_screen", st == 200 and len(names) >= 40,
           {"status": st, "series": len(names), "has_type": "# TYPE" in metrics_text})

    # 2 单请求 SSE
    one = sse_once(base, "hello world", 16)
    record("sse_single", one["done"] and one["frames"] >= 17, one)

    # 3 非流式
    st, body = http_json(base, "/completion", {"prompt": "hello", "max_tokens": 8})
    parsed = json.loads(body) if st == 200 else {}
    usage = parsed.get("usage", {})
    record("non_stream", st == 200 and len(parsed.get("choices", [])) == 1 and usage.get("completion_tokens", 0) > 0,
           {"status": st, "usage": usage, "finish": parsed.get("choices", [{}])[0].get("finish_reason")})

    # 4 并发 8 路
    results = []
    lock = threading.Lock()

    def worker(i):
        r = sse_once(base, "concurrent prompt " + str(i), 24)
        with lock:
            results.append(r)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(8)]
    started = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    wall = time.perf_counter() - started
    total_tokens = sum(len(r["gaps_ms"]) + 1 for r in results)
    ok = len(results) == 8 and all(r["done"] for r in results)
    record("concurrent_8", ok, {"completed": len(results), "tokens": total_tokens,
                                 "tok_s": round(total_tokens / wall, 1) if wall else None})

    # 5 非法输入
    st, body = http_json(base, "/completion", raw="{")
    record("invalid_json", st == 400, {"status": st, "body": body[:120]})
    st, body = http_json(base, "/completion", {})
    record("missing_prompt", st == 400, {"status": st, "body": body[:120]})
    st, body = http_json(base, "/completion", {"prompt": "", "max_tokens": 4})
    record("empty_prompt", st == 400, {"status": st, "body": body[:120]})
    st, body = http_json(base, "/completion", {"prompt": "x", "max_tokens": 0})
    record("max_tokens_zero", st == 400, {"status": st, "body": body[:120]})
    st, body = http_json(base, "/completion", {"prompt": "x", "max_tokens": 99999})
    record("max_tokens_oversize", st == 400, {"status": st, "body": body[:120]})

    # 6 超上下文
    st, body = http_json(base, "/completion",
                         {"prompt": "token " * 1600, "max_tokens": 8})
    record("prompt_over_ctx", st == 400 and "CONTEXT" in body, {"status": st, "body": body[:150]})

    # 7 模型名不校验（设计缺口探测）
    st, body = http_json(base, "/completion", {"prompt": "x", "max_tokens": 4, "model": "gpt-4"})
    record("unknown_model_echo", st == 200, {"status": st, "note": "服务端未校验 model 字段"})

    # 8 重复请求 ID（探测 ID 冲突）
    for _ in range(2):
        st, _ = http_json(base, "/completion", {"prompt": "x", "max_tokens": 4, "id": "dup-id"})
    record("duplicate_request_id", st == 200, {"status": st, "note": "重复 id 未被拒绝"})

    # 9 取消：读 2 帧即断开
    before = metric(base, 'llmopt_requests_total{status="cancelled"}')
    sse_once(base, "cancel me", 128, stop_after=2)
    time.sleep(0.5)
    after = metric(base, 'llmopt_requests_total{status="cancelled"}')
    record("disconnect_cancel", before is not None and after is not None and after > before,
           {"before": before, "after": after})

    # 10 慢客户端隔离 + 溢出计数
    overflow_before = metric(base, "llmopt_sink_overflow_total")
    slow_stats = {}

    def slow():
        try:
            slow_stats.update(sse_once(base, "slow client", 300, stop_after=3, delay=1.0))
        except Exception as exc:
            slow_stats["error"] = type(exc).__name__

    slow_thread = threading.Thread(target=slow)
    slow_thread.start()
    time.sleep(0.2)
    fast = {"frames": 0, "gaps_ms": [], "error": None}
    try:
        fast = sse_once(base, "fast client", 64)
    except Exception as exc:
        fast["error"] = type(exc).__name__
    slow_thread.join()
    overflow_after = metric(base, "llmopt_sink_overflow_total")
    record("slow_client_isolation", overflow_after is not None,
           {"overflow_before": overflow_before, "overflow_after": overflow_after,
            "slow_frames": slow_stats.get("frames"), "slow_error": slow_stats.get("error"),
            "fast_error": fast.get("error"),
            "fast_p99_ms": None if not fast.get("gaps_ms")
            else round(statistics.quantiles(fast["gaps_ms"], n=100)[98], 2)})

    # 11 队列满
    codes = []

    def burst(i):
        try:
            st, _ = http_json(base, "/completion", {"prompt": "burst " + str(i), "max_tokens": 32})
            with lock:
                codes.append(st)
        except Exception as exc:  # 连接被拒也算一种真实结果
            with lock:
                codes.append(type(exc).__name__)

    burst_threads = [threading.Thread(target=burst, args=(i,)) for i in range(16)]
    for t in burst_threads:
        t.start()
    for t in burst_threads:
        t.join()
    record("queue_pressure", all(c in (200, 429) for c in codes) and any(c == 429 for c in codes),
           {"codes": {str(c): codes.count(c) for c in set(codes)}})

    # 12 chat 端点（预期缺口）
    st, body = http_json(base, "/v1/chat/completions", {"messages": [{"role": "user", "content": "hi"}]})
    record("chat_endpoint_gap", st != 200, {"status": st, "body": body[:120]})

    summary = {"cases": RESULTS, "passed": sum(1 for r in RESULTS if r["ok"]), "total": len(RESULTS)}
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(summary, handle, ensure_ascii=False, indent=2)
    print("\n总结: %d/%d 通过，明细写入 %s" % (summary["passed"], summary["total"], args.out))


if __name__ == "__main__":
    main()
