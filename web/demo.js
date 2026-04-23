const PAGE_COMPARE_PATH = "/api/v1/page?user_id=1&lat=37.5&lng=127.0&mode=compare&delay_ms=10";
const CACHE_TTL_WAIT_MAX_MS = 7600;
const CACHE_TTL_POLL_MS = 250;

const STEP_DEFS = [
  {
    id: "health",
    label: "1단계",
    title: "서버 상태 확인",
    situation: "시연 시작 전 서버 정상 여부 확인",
    request: "GET /api/v1/health",
    expected: "status=up",
    runner: runHealthStep
  },
  {
    id: "compare",
    label: "2단계",
    title: "순차/병렬 실제 대조",
    situation: "같은 입력에서 sequential과 parallel을 실제 비교",
    request: "GET /api/v1/page?mode=compare&delay_ms=10",
    expected: "speedup 수치와 start_ms/lat_ms 확인",
    runner: runPageCompareStep
  },
  {
    id: "cache",
    label: "3단계",
    title: "캐시 hit/miss 시연",
    situation: "속도와 무효화(no_entry/version/ttl)를 직관적으로 확인",
    request: "POST /api/v1/sql 반복 + UPDATE + TTL 대기",
    expected: "miss -> hit -> miss(version_changed) -> miss(ttl_expired)",
    runner: runCacheStep
  },
  {
    id: "rollback",
    label: "4단계",
    title: "트랜잭션 롤백 비교",
    situation: "실패 시 데이터가 그대로 유지되는지 확인",
    request: "POST /api/v1/tx 실패 유도",
    expected: "TX_ABORT + 전/후 동일",
    runner: runRollbackStep
  },
  {
    id: "threads",
    label: "5단계",
    title: "동시 요청 수별 성능 그래프",
    situation: "실사용처럼 동시 요청 8/16/32에서 처리 성능 비교",
    request: "GET /api/v1/page 요청을 concurrency N(8/16/32)으로 실행",
    expected: "동시 요청 증가 시 batch 시간/p95 변화 확인",
    runner: runThreadCompareStep
  },
  {
    id: "load",
    label: "6단계",
    title: "부하 테스트 요약",
    situation: "대량 요청 시 API 안정성과 지연 분포 확인",
    request: "브라우저 병렬 요청(load mini test) 실행",
    expected: "성공률, RPS, p95, metrics 증감 표시",
    runner: runLoadStep
  }
];

const state = {
  running: false,
  refs: {}
};

function el(id) {
  return document.getElementById(id);
}

function nowStamp() {
  const d = new Date();
  const pad = (n) => String(n).padStart(2, "0");
  return pad(d.getHours()) + ":" + pad(d.getMinutes()) + ":" + pad(d.getSeconds());
}

function logLine(message) {
  const out = el("console-output");
  out.textContent += "\n[" + nowStamp() + "] " + message;
  out.scrollTop = out.scrollHeight;
}

function setServerBadge(text, kind) {
  const badge = el("server-badge");
  badge.textContent = text;
  badge.className = "badge " + (kind || "");
}

function setStepState(ref, label, kind) {
  ref.state.textContent = label;
  ref.state.className = "state-chip" + (kind ? " " + kind : "");
}

function setStepLatency(ref, ms) {
  ref.latency.textContent = ms + " ms";
}

function setJson(ref, value) {
  ref.json.textContent = JSON.stringify(value, null, 2);
}

function escapeHtml(text) {
  return String(text)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

function renderResult(ref, html) {
  ref.result.innerHTML = html;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function apiSummary(res) {
  if (res.payload && res.payload.err && res.payload.err.code) {
    return "HTTP " + res.status + " (" + res.payload.err.code + ")";
  }
  return "HTTP " + res.status;
}

function cacheMetaOf(res) {
  const meta = res && res.payload && res.payload.meta ? res.payload.meta : {};
  return meta.cache || { status: "-", reason: "-" };
}

function serverLatOf(res) {
  const meta = res && res.payload && res.payload.meta ? res.payload.meta : {};
  const n = Number(meta.lat_ms);
  return Number.isFinite(n) ? n : null;
}

function serverLatUsOf(res) {
  const meta = res && res.payload && res.payload.meta ? res.payload.meta : {};
  const n = Number(meta.lat_us);
  return Number.isFinite(n) ? n : null;
}

async function requestApi(method, path, body) {
  const options = { method };
  const started = performance.now();

  if (body !== undefined) {
    options.headers = { "Content-Type": "application/json" };
    options.body = JSON.stringify(body);
  }

  const response = await fetch(path, options);
  const raw = await response.text();
  const elapsed = Math.round(performance.now() - started);
  let payload = raw;

  if ((response.headers.get("content-type") || "").includes("application/json")) {
    try {
      payload = JSON.parse(raw);
    } catch (e) {
      payload = { parse_error: true, raw };
    }
  }

  return { status: response.status, raw, payload, elapsed };
}

function sqlRequest(query) {
  return requestApi("POST", "/api/v1/sql", { query });
}

function cartRowOf(payload) {
  const rows = payload && payload.data && Array.isArray(payload.data.rows) ? payload.data.rows : [];
  return rows.length > 0 ? rows[0] : null;
}

function traceLineOf(row) {
  const name = String(row && row.name ? row.name : "-").padEnd(11, " ");
  const tid = row && row.thr_id != null ? row.thr_id : "-";
  const start = row && row.start_ms != null ? row.start_ms : "-";
  const lat = row && row.lat_ms != null ? row.lat_ms : "-";
  return name + " thr_id: " + tid + " start_ms: " + start + " lat_ms: " + lat;
}

function traceBlock(title, trace, sumLat, totalMs, speedup) {
  const lines = [];
  lines.push("[" + title + "]");
  (Array.isArray(trace) ? trace : []).forEach((row) => {
    lines.push(traceLineOf(row));
  });
  if (sumLat != null) lines.push("sum_lat_ms: " + sumLat);
  lines.push("total_ms: " + totalMs);
  if (speedup != null) lines.push("speedup: " + speedup.toFixed(1) + "x");
  return lines.join("\n");
}

function percentile(values, p) {
  if (!values || values.length === 0) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const idx = Math.min(sorted.length - 1, Math.max(0, Math.ceil((p / 100) * sorted.length) - 1));
  return sorted[idx];
}

async function runStep(def) {
  if (state.running) return;
  state.running = true;
  const ref = state.refs[def.id];
  const started = performance.now();

  Object.values(state.refs).forEach((item) => {
    item.button.disabled = true;
  });
  setStepState(ref, "실행 중", "running");
  setStepLatency(ref, 0);
  renderResult(ref, "<p class=\"kv-line\">처리 중...</p>");

  try {
    const out = await def.runner(ref);
    const elapsed = Math.round(performance.now() - started);
    setStepLatency(ref, elapsed);
    setStepState(ref, out.ok ? "성공" : "실패", out.ok ? "ok" : "fail");
    setJson(ref, out.json);
    logLine(def.label + " " + def.title + ": " + out.log);
  } catch (err) {
    const elapsed = Math.round(performance.now() - started);
    const msg = err && err.message ? err.message : "unknown error";
    setStepLatency(ref, elapsed);
    setStepState(ref, "실패", "fail");
    renderResult(
      ref,
      "<p class=\"kv-line\"><strong>오류</strong>" + escapeHtml(msg) + "</p>"
    );
    setJson(ref, { error: msg });
    logLine(def.label + " " + def.title + ": 실패 - " + msg);
  } finally {
    Object.values(state.refs).forEach((item) => {
      item.button.disabled = false;
    });
    state.running = false;
  }
}

async function runHealthStep(ref) {
  const res = await requestApi("GET", "/api/v1/health");
  const up = res.status === 200 && res.payload && res.payload.data && res.payload.data.status === "up";

  renderResult(
    ref,
    [
      "<h3 class=\"result-title\">실시간 상태</h3>",
      "<div class=\"kv-list\">",
      "<p class=\"kv-line\"><strong>결과</strong>" + (up ? "정상 응답" : "응답 오류") + "</p>",
      "<p class=\"kv-line\"><strong>요약</strong>" + apiSummary(res) + "</p>",
      "</div>"
    ].join("")
  );

  setServerBadge(up ? "서버 상태: 정상" : "서버 상태: 오류", up ? "ok" : "fail");

  return {
    ok: up,
    log: up ? "서버 상태 정상 확인" : "서버 상태 비정상",
    json: {
      request: { method: "GET", path: "/api/v1/health" },
      response: res.payload,
      status: res.status,
      latency_ms: res.elapsed
    }
  };
}

async function runPageCompareStep(ref) {
  const res = await requestApi("GET", PAGE_COMPARE_PATH);
  const data = res.payload && res.payload.data ? res.payload.data : {};
  const seq = data.sequential || {};
  const par = data.parallel || {};
  const speedup = Number(data.speedup || 0);
  const ok = res.status === 200 && speedup > 1.0;

  const seqBlock = traceBlock(
    "page sequential",
    seq.trace,
    seq.sum_lat_ms,
    seq.total_ms
  );
  const parBlock = traceBlock(
    "page parallel",
    par.trace,
    par.sum_lat_ms,
    par.total_ms,
    speedup
  );

  renderResult(
    ref,
    [
      "<h3 class=\"result-title\">실제 대조군 비교</h3>",
      "<div class=\"compare-grid\">",
      "<pre class=\"plain-block\">" + escapeHtml(seqBlock) + "</pre>",
      "<pre class=\"plain-block\">" + escapeHtml(parBlock) + "</pre>",
      "</div>",
      "<p class=\"kv-line\"><strong>요약</strong>speedup " + speedup.toFixed(1) + "x</p>"
    ].join("")
  );

  return {
    ok,
    log: "순차 " + seq.total_ms + "ms, 병렬 " + par.total_ms + "ms, speedup " + speedup.toFixed(1) + "x",
    json: {
      request: { method: "GET", path: PAGE_COMPARE_PATH },
      response: res.payload,
      status: res.status
    }
  };
}

function cacheRow(label, res, note) {
  const meta = cacheMetaOf(res);
  const serverLat = serverLatOf(res);
  const serverLatUs = serverLatUsOf(res);
  const latText =
    serverLat == null
      ? "-"
      : serverLatUs == null
        ? String(serverLat) + "ms"
        : String(serverLat) + "ms (" + String(serverLatUs) + "us)";
  return (
    "<div class=\"cache-row\">" +
      "<p>" + escapeHtml(label) + "</p>" +
      "<p>" + escapeHtml(meta.status || "-") + "</p>" +
      "<p>" + escapeHtml(meta.reason || "-") + "</p>" +
      "<p>" + latText + "</p>" +
      "<p>" + escapeHtml(note || "-") + "</p>" +
    "</div>"
  );
}

async function runCacheStep(ref) {
  const baseQuery = "SELECT * FROM restaurants WHERE id = 1" + " ".repeat((Date.now() % 997) + 1);
  const firstRes = await sqlRequest(baseQuery);
  const secondRes = await sqlRequest(baseQuery);
  const beforeRows = firstRes.payload && firstRes.payload.data ? firstRes.payload.data.rows : [];
  const before = beforeRows && beforeRows[0] ? beforeRows[0] : null;
  const fromStatus = before && before.status ? String(before.status).toLowerCase() : "open";
  const toStatus = fromStatus === "open" ? "closed" : "open";
  const updateRes = await sqlRequest("UPDATE restaurants SET status = '" + toStatus + "' WHERE id = 1");
  const afterUpdateRes = await sqlRequest(baseQuery);

  const waitStart = performance.now();
  let ttlRes = afterUpdateRes;
  while (performance.now() - waitStart < CACHE_TTL_WAIT_MAX_MS) {
    const meta = cacheMetaOf(ttlRes);
    if (meta.reason === "ttl_expired") break;
    await sleep(CACHE_TTL_POLL_MS);
    ttlRes = await sqlRequest(baseQuery);
  }
  const ttlWaitMs = Math.round(performance.now() - waitStart);
  const ttlMeta = cacheMetaOf(ttlRes);

  const ok =
    cacheMetaOf(firstRes).status === "miss" &&
    cacheMetaOf(secondRes).status === "hit" &&
    cacheMetaOf(afterUpdateRes).reason === "version_changed" &&
    ttlMeta.reason === "ttl_expired";

  renderResult(
    ref,
    [
      "<h3 class=\"result-title\">캐시 동작 흐름</h3>",
      "<div class=\"cache-grid\">",
      "<div class=\"cache-row head\"><p>step</p><p>status</p><p>reason</p><p>lat</p><p>note</p></div>",
      cacheRow("step1 first select", firstRes, "초기 조회"),
      cacheRow("step2 same select", secondRes, "동일 요청 재호출"),
      cacheRow("step3 update table", updateRes, "status " + fromStatus + "→" + toStatus),
      cacheRow("step4 same select", afterUpdateRes, "table version 변경"),
      cacheRow("step5 wait", { payload: { meta: { cache: { status: "-", reason: "-" } } } }, "TTL 대기 " + ttlWaitMs + "ms"),
      cacheRow("step6 same select", ttlRes, "TTL 만료 확인"),
      "</div>",
      "<p class=\"kv-line\"><strong>측정 기준</strong>lat은 서버 meta.lat_ms / meta.lat_us 기준 (브라우저 왕복시간 제외)</p>",
      "<p class=\"kv-line\"><strong>요약</strong>miss → hit → miss(version_changed) → miss(ttl_expired)</p>"
    ].join("")
  );

  return {
    ok,
    log: "캐시 흐름: step1 miss, step2 hit, step4 version miss, step6 ttl miss",
    json: {
      query: baseQuery.trim(),
      steps: [
        {
          step: 1,
          response: firstRes.payload,
          status: firstRes.status,
          server_lat_ms: serverLatOf(firstRes),
          server_lat_us: serverLatUsOf(firstRes),
          client_elapsed_ms: firstRes.elapsed
        },
        {
          step: 2,
          response: secondRes.payload,
          status: secondRes.status,
          server_lat_ms: serverLatOf(secondRes),
          server_lat_us: serverLatUsOf(secondRes),
          client_elapsed_ms: secondRes.elapsed
        },
        {
          step: 3,
          response: updateRes.payload,
          status: updateRes.status,
          server_lat_ms: serverLatOf(updateRes),
          server_lat_us: serverLatUsOf(updateRes),
          client_elapsed_ms: updateRes.elapsed
        },
        {
          step: 4,
          response: afterUpdateRes.payload,
          status: afterUpdateRes.status,
          server_lat_ms: serverLatOf(afterUpdateRes),
          server_lat_us: serverLatUsOf(afterUpdateRes),
          client_elapsed_ms: afterUpdateRes.elapsed
        },
        {
          step: 6,
          response: ttlRes.payload,
          status: ttlRes.status,
          server_lat_ms: serverLatOf(ttlRes),
          server_lat_us: serverLatUsOf(ttlRes),
          client_elapsed_ms: ttlRes.elapsed,
          ttl_wait_ms: ttlWaitMs
        }
      ]
    }
  };
}

async function runRollbackStep(ref) {
  const selectQuery = "SELECT * FROM cart WHERE user_id = '1'";
  const txBody = {
    queries: [
      "INSERT INTO cart VALUES (9, 9, 8, 99999)",
      "INVALID SQL"
    ]
  };
  const beforeRes = await sqlRequest(selectQuery);
  const txRes = await requestApi("POST", "/api/v1/tx", txBody);
  const afterRes = await sqlRequest(selectQuery);
  const before = cartRowOf(beforeRes.payload);
  const after = cartRowOf(afterRes.payload);
  const unchanged = JSON.stringify(before || {}) === JSON.stringify(after || {});
  const rolled = txRes.payload && txRes.payload.err && txRes.payload.err.code === "TX_ABORT";
  const ok = rolled && unchanged;

  renderResult(
    ref,
    [
      "<h3 class=\"result-title\">롤백 전/후 비교</h3>",
      "<div class=\"rollback-grid\">",
      "<article class=\"data-card\">",
      "<h4>실행 전</h4>",
      "<p class=\"kv-line\"><strong>user_id</strong>" + escapeHtml(before ? before.user_id : "-") + "</p>",
      "<p class=\"kv-line\"><strong>item_count</strong>" + escapeHtml(before ? before.item_count : "-") + "</p>",
      "<p class=\"kv-line\"><strong>total</strong>" + escapeHtml(before ? before.total : "-") + "</p>",
      "</article>",
      "<div class=\"arrow\">→</div>",
      "<article class=\"data-card\">",
      "<h4>실행 후</h4>",
      "<p class=\"kv-line\"><strong>user_id</strong>" + escapeHtml(after ? after.user_id : "-") + "</p>",
      "<p class=\"kv-line\"><strong>item_count</strong>" + escapeHtml(after ? after.item_count : "-") + "</p>",
      "<p class=\"kv-line\"><strong>total</strong>" + escapeHtml(after ? after.total : "-") + "</p>",
      "</article>",
      "</div>",
      "<div class=\"" + (ok ? "rollback-ok" : "rollback-fail") + "\">" +
      (ok ? "ROLLBACK 유지됨 (전/후 동일)" : "ROLLBACK 확인 필요") +
      "</div>",
      "<p class=\"kv-line\"><strong>TX 결과</strong>" + apiSummary(txRes) + "</p>"
    ].join("")
  );

  return {
    ok,
    log: ok ? "롤백 성공: 전/후 데이터 동일" : "롤백 검증 실패",
    json: {
      request_flow: [
        { method: "POST", path: "/api/v1/sql", query: selectQuery },
        { method: "POST", path: "/api/v1/tx", body: txBody },
        { method: "POST", path: "/api/v1/sql", query: selectQuery }
      ],
      before: beforeRes.payload,
      tx: txRes.payload,
      after: afterRes.payload,
      status: [beforeRes.status, txRes.status, afterRes.status]
    }
  };
}

async function runThreadCompareStep(ref) {
  const workersList = [8, 16, 32];
  const targetRequestsPerLevel = 64;
  const delayMs = 10;
  const rows = [];

  for (const concurrency of workersList) {
    const repeats = Math.max(1, Math.ceil(targetRequestsPerLevel / concurrency));
    const batchLatencies = [];
    const reqLatencies = [];
    let success = 0;
    let fail = 0;
    let qfull = 0;
    const startedAll = performance.now();
    for (let i = 0; i < repeats; i++) {
      const started = performance.now();
      const calls = Array.from({ length: concurrency }, () =>
        requestApi(
          "GET",
          "/api/v1/page?user_id=1&lat=37.5&lng=127.0&mode=parallel&workers=4&delay_ms=" + delayMs
        )
      );
      const responses = await Promise.all(calls);
      batchLatencies.push(Math.round(performance.now() - started));
      responses.forEach((res) => {
        reqLatencies.push(res.elapsed);
        if (res.status === 200) {
          success++;
        } else {
          fail++;
          if (res && res.payload && res.payload.err && res.payload.err.code === "Q_FULL") qfull++;
        }
      });
    }
    const elapsedAllMs = Math.max(1, Math.round(performance.now() - startedAll));
    const totalRequests = success + fail;
    const avg = Math.round(batchLatencies.reduce((sum, x) => sum + x, 0) / repeats);
    const p95 = Math.round(percentile(reqLatencies, 95));
    const rps = Number((totalRequests / (elapsedAllMs / 1000)).toFixed(1));
    const failRate = totalRequests === 0 ? 0 : Number(((fail / totalRequests) * 100).toFixed(2));
    rows.push({
      concurrency,
      repeats,
      requests: totalRequests,
      total_ms: elapsedAllMs,
      avg_batch_ms: avg,
      p95_ms: p95,
      rps,
      fail_rate_pct: failRate,
      qfull
    });
  }

  const maxTotal = rows.reduce((m, r) => (r.total_ms > m ? r.total_ms : m), 1);
  const graphRows = rows
    .map((r) => {
      const width = Math.max(8, Math.round((r.total_ms / maxTotal) * 100));
      return (
        "<div class=\"thread-row\">" +
          "<p>concurrency " + r.concurrency + "</p>" +
          "<div class=\"thread-bar-bg\"><div class=\"thread-bar\" style=\"width:" + width + "%\"></div></div>" +
          "<p>total " + r.total_ms + "ms / p95 " + r.p95_ms + "ms / " + r.rps + " rps</p>" +
        "</div>"
      );
    })
    .join("");

  renderResult(
    ref,
    [
      "<h3 class=\"result-title\">동시 요청 수별 비교 (동일 총 요청 기준)</h3>",
      "<div class=\"thread-graph\">",
      graphRows,
      "</div>",
      "<p class=\"kv-line\"><strong>요약</strong>각 구간 동일 요청 수(약 " + targetRequestsPerLevel + ") 기준으로 p95/처리량/실패율 비교</p>"
    ].join("")
  );

  return {
    ok: rows.length === workersList.length,
    log: "concurrency 8/16/32: " + rows.map((r) => r.concurrency + "(total " + r.total_ms + "ms, p95 " + r.p95_ms + "ms, rps " + r.rps + ", fail " + r.fail_rate_pct + "%)").join(", "),
    json: {
      mode: "parallel_page_concurrency",
      delay_ms: delayMs,
      target_requests_per_level: targetRequestsPerLevel,
      results: rows
    }
  };
}

function toNumber(value) {
  const n = Number(value || 0);
  return Number.isFinite(n) ? n : 0;
}

async function runLoadStep(ref) {
  const targetRequests = 5000;
  const vus = 16;
  const maxRetries = 2;
  const mix = { read_sql_pct: 50, write_sql_pct: 30, read_page_pct: 20 };
  const loops = Math.ceil(targetRequests / Math.max(1, vus));
  const totalPlanned = targetRequests;
  const latencies = [];
  let success = 0;
  let fail = 0;
  let issued = 0;
  let completed = 0;
  let lastPaint = performance.now();
  const mixCount = {
    read_sql: { total: 0, success: 0, fail: 0 },
    write_sql: { total: 0, success: 0, fail: 0 },
    read_page: { total: 0, success: 0, fail: 0 }
  };

  const beforeMetrics = await requestApi("GET", "/api/v1/metrics");

  const callWithRetry = async (method, path, body) => {
    for (let attempt = 0; attempt <= maxRetries; attempt++) {
      const res = await requestApi(method, path, body);
      const errCode = res && res.payload && res.payload.err ? String(res.payload.err.code || "") : "";
      const retriable = res.status === 503 || errCode === "Q_FULL";
      if (!retriable || attempt === maxRetries) return res;
      await sleep(5 * (attempt + 1));
    }
    return requestApi(method, path, body);
  };

  const started = performance.now();
  const renderProgress = (force) => {
    const now = performance.now();
    if (!force && now - lastPaint < 350) return;
    lastPaint = now;
    const pct = targetRequests === 0 ? 0 : Math.min(100, Math.round((completed / targetRequests) * 100));
    const elapsed = Math.max(1, Math.round(now - started));
    const rpsNow = Number((completed / (elapsed / 1000)).toFixed(1));
    const successRateNow = completed === 0 ? 0 : Number(((success / completed) * 100).toFixed(1));
    renderResult(
      ref,
      [
        "<h3 class=\"result-title\">브라우저 부하 테스트 진행 중</h3>",
        "<div class=\"load-kpis\">",
        "<article class=\"mini\"><p class=\"label\">진행</p><p class=\"value\">" + pct + "%</p></article>",
        "<article class=\"mini\"><p class=\"label\">완료</p><p class=\"value\">" + completed + " / " + targetRequests + "</p></article>",
        "<article class=\"mini\"><p class=\"label\">성공률</p><p class=\"value\">" + successRateNow + "%</p></article>",
        "<article class=\"mini\"><p class=\"label\">현재 RPS</p><p class=\"value\">" + rpsNow + "</p></article>",
        "</div>",
        "<p class=\"kv-line\"><strong>설정</strong>target " + targetRequests + ", vus " + vus + "</p>"
      ].join("")
    );
  };

  renderProgress(true);
  const workers = Array.from({ length: vus }, (() => async () => {
    while (true) {
      let seq;
      let res;
      let kind;

      if (issued >= targetRequests) break;
      seq = issued;
      issued += 1;

      if ((seq % 10) < 5) {
        kind = "read_sql";
        const id = (seq % 13) + 1;
        res = await callWithRetry(
          "POST",
          "/api/v1/sql",
          { query: "SELECT * FROM restaurants WHERE id = " + id }
        );
      } else if ((seq % 10) < 8) {
        kind = "write_sql";
        const id = (seq % 4) + 1;
        const status = (seq % 2) === 0 ? "open" : "closed";
        res = await callWithRetry(
          "POST",
          "/api/v1/sql",
          { query: "UPDATE restaurants SET status = '" + status + "' WHERE id = " + id }
        );
      } else {
        kind = "read_page";
        res = await callWithRetry(
          "GET",
          "/api/v1/page?user_id=1&lat=37.5&lng=127.0&mode=parallel&workers=4&delay_ms=5"
        );
      }

      latencies.push(res.elapsed);
      mixCount[kind].total++;
      if (res.status === 200) {
        success++;
        mixCount[kind].success++;
      } else {
        fail++;
        mixCount[kind].fail++;
      }
      completed++;
      renderProgress(false);
    }
  })());

  await Promise.all(workers);
  renderProgress(true);
  const elapsedMs = Math.max(1, Math.round(performance.now() - started));
  const afterMetrics = await requestApi("GET", "/api/v1/metrics");

  const count = success + fail;
  const avg = count === 0 ? 0 : Math.round(latencies.reduce((s, v) => s + v, 0) / count);
  const p95 = Math.round(percentile(latencies, 95));
  const p99 = Math.round(percentile(latencies, 99));
  const rps = Number((count / (elapsedMs / 1000)).toFixed(1));
  const successRate = count === 0 ? 0 : Number(((success / count) * 100).toFixed(1));
  const ok = fail === 0;

  const beforeData = beforeMetrics.payload && beforeMetrics.payload.data ? beforeMetrics.payload.data : {};
  const afterData = afterMetrics.payload && afterMetrics.payload.data ? afterMetrics.payload.data : {};
  const reqDelta = toNumber(afterData.http && afterData.http.total_requests) - toNumber(beforeData.http && beforeData.http.total_requests);
  const errDelta = toNumber(afterData.http && afterData.http.error_responses) - toNumber(beforeData.http && beforeData.http.error_responses);
  const q503Delta = toNumber(afterData.http && afterData.http.status_503) - toNumber(beforeData.http && beforeData.http.status_503);

  renderResult(
    ref,
    [
      "<h3 class=\"result-title\">브라우저 부하 테스트 결과</h3>",
      "<div class=\"load-kpis\">",
      "<article class=\"mini\"><p class=\"label\">요청 수</p><p class=\"value\">" + count + "</p></article>",
      "<article class=\"mini\"><p class=\"label\">성공률</p><p class=\"value\">" + successRate + "%</p></article>",
      "<article class=\"mini\"><p class=\"label\">RPS</p><p class=\"value\">" + rps + "</p></article>",
      "<article class=\"mini\"><p class=\"label\">p95</p><p class=\"value\">" + p95 + "ms</p></article>",
      "</div>",
      "<div class=\"load-grid\">",
      "<p class=\"kv-line\"><strong>req mix</strong>SELECT /api/v1/sql 50%, UPDATE /api/v1/sql 30%, GET /api/v1/page 20%</p>",
      "<p class=\"kv-line\"><strong>avg/p99</strong>" + avg + "ms / " + p99 + "ms</p>",
      "<p class=\"kv-line\"><strong>metrics Δrequests</strong>" + reqDelta + "</p>",
      "<p class=\"kv-line\"><strong>metrics Δerrors</strong>" + errDelta + " (503 Δ" + q503Delta + ")</p>",
      "<p class=\"kv-line\"><strong>시나리오</strong>vus " + vus + ", loops " + loops + ", planned " + totalPlanned + "</p>",
      "</div>"
    ].join("")
  );

  return {
    ok,
    log: "부하테스트 완료: success " + success + "/" + count + ", p95 " + p95 + "ms, rps " + rps,
    json: {
      config: { vus, target_requests: targetRequests, mix },
      summary: {
        total: count,
        success,
        fail,
        success_rate: successRate,
        elapsed_ms: elapsedMs,
        rps,
        avg_ms: avg,
        p95_ms: p95,
        p99_ms: p99
      },
      metrics_delta: {
        total_requests: reqDelta,
        error_responses: errDelta,
        status_503: q503Delta
      },
      mix_breakdown: mixCount
    }
  };
}

function createStepCard(def) {
  const card = document.createElement("section");
  const head = document.createElement("div");
  const label = document.createElement("p");
  const title = document.createElement("h2");
  const lines = document.createElement("div");
  const runRow = document.createElement("div");
  const runBtn = document.createElement("button");
  const stateChip = document.createElement("span");
  const latency = document.createElement("span");
  const result = document.createElement("div");
  const details = document.createElement("details");
  const summary = document.createElement("summary");
  const json = document.createElement("pre");

  card.className = "scenario-step";
  head.className = "step-head";
  label.className = "step-label";
  label.textContent = def.label;
  title.className = "step-title";
  title.textContent = def.title;
  head.appendChild(label);
  head.appendChild(title);

  lines.className = "step-lines";
  lines.innerHTML = [
    "<p class=\"step-line\"><strong>상황</strong>" + escapeHtml(def.situation) + "</p>",
    "<p class=\"step-line\"><strong>요청</strong>" + escapeHtml(def.request) + "</p>",
    "<p class=\"step-line\"><strong>기대</strong>" + escapeHtml(def.expected) + "</p>"
  ].join("");

  runRow.className = "run-row";
  runBtn.className = "run-btn";
  runBtn.type = "button";
  runBtn.textContent = "실행";
  stateChip.className = "state-chip";
  stateChip.textContent = "대기";
  latency.className = "latency";
  latency.textContent = "0 ms";
  runRow.appendChild(runBtn);
  runRow.appendChild(stateChip);
  runRow.appendChild(latency);

  result.className = "result-view";
  result.innerHTML = "<p class=\"kv-line\">실행 전입니다.</p>";

  details.className = "json-details";
  summary.textContent = "JSON 보기";
  json.className = "code";
  json.textContent = "{}";
  details.appendChild(summary);
  details.appendChild(json);

  card.appendChild(head);
  card.appendChild(lines);
  card.appendChild(runRow);
  card.appendChild(result);
  card.appendChild(details);

  runBtn.addEventListener("click", () => runStep(def));
  state.refs[def.id] = {
    button: runBtn,
    state: stateChip,
    latency,
    result,
    json
  };
  return card;
}

function init() {
  const root = el("scenario-list");
  root.textContent = "";
  STEP_DEFS.forEach((def) => {
    root.appendChild(createStepCard(def));
  });

  el("console-clear").addEventListener("click", () => {
    el("console-output").textContent = "[대기] 로그를 초기화했습니다.";
  });
}

init();
