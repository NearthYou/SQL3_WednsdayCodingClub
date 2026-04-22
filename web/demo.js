const PAGE_PATH = "/api/v1/page?user_id=1&lat=37.5&lng=127.0";
const STEP_DEFS = [
  {
    id: "health",
    label: "1단계",
    title: "서버 상태 확인",
    situation: "시연 시작 전에 서버가 살아있는지 확인",
    request: "GET /api/v1/health",
    expected: "정상일 때 status=up",
    runner: runHealthStep
  },
  {
    id: "toggle",
    label: "2단계",
    title: "상태 변경 토글",
    situation: "데이터가 실제로 바뀌는 장면을 바로 보여주기",
    request: "UPDATE 후 SELECT 재조회",
    expected: "status 배지가 open/closed로 즉시 변경",
    runner: runToggleStep
  },
  {
    id: "race",
    label: "3단계",
    title: "병렬 요청 레이스",
    situation: "동시성 처리 결과를 한눈에 확인",
    request: "GET /api/v1/page 20개 동시 실행",
    expected: "완료 순서/지연시간/스레드 분포 표시",
    runner: runConcurrencyRace
  },
  {
    id: "rollback",
    label: "4단계",
    title: "트랜잭션 롤백 비교",
    situation: "실패 시 데이터가 유지되는지 직관적으로 확인",
    request: "TX 실패 유도 후 실행 전/후 비교",
    expected: "전/후 동일, TX_ABORT 표시",
    runner: runRollbackStep
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

function apiSummary(res) {
  if (res.payload && res.payload.err && res.payload.err.code) {
    return "HTTP " + res.status + " (" + res.payload.err.code + ")";
  }
  return "HTTP " + res.status;
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

async function runToggleStep(ref) {
  const selectQuery = "SELECT * FROM restaurants WHERE id = 1";
  const beforeRes = await sqlRequest(selectQuery);
  const beforeRows = beforeRes.payload && beforeRes.payload.data ? beforeRes.payload.data.rows : [];
  const before = beforeRows && beforeRows[0] ? beforeRows[0] : null;

  if (!before || !before.status) {
    renderResult(
      ref,
      "<p class=\"kv-line\"><strong>오류</strong>대상 데이터를 읽지 못했습니다.</p>"
    );
    return {
      ok: false,
      log: "초기 조회 실패 (" + apiSummary(beforeRes) + ")",
      json: { before: beforeRes.payload, status: beforeRes.status }
    };
  }

  const fromStatus = String(before.status).toLowerCase();
  const toStatus = fromStatus === "open" ? "closed" : "open";
  const updateQuery = "UPDATE restaurants SET status = '" + toStatus + "' WHERE id = 1";
  const updateRes = await sqlRequest(updateQuery);
  const afterRes = await sqlRequest(selectQuery);
  const afterRows = afterRes.payload && afterRes.payload.data ? afterRes.payload.data.rows : [];
  const after = afterRows && afterRows[0] ? afterRows[0] : null;
  const afterStatus = after && after.status ? String(after.status).toLowerCase() : "";
  const ok = updateRes.status === 200 && afterRes.status === 200 && afterStatus === toStatus;

  renderResult(
    ref,
    [
      "<h3 class=\"result-title\">상태 변경 결과</h3>",
      "<div class=\"toggle-grid\">",
      "<article class=\"data-card\">",
      "<h4>변경 전</h4>",
      "<p class=\"kv-line\"><strong>매장</strong>" + escapeHtml(before.name || "unknown") + "</p>",
      "<span class=\"status-badge " + (fromStatus === "open" ? "status-open" : "status-closed") + "\">" + escapeHtml(fromStatus) + "</span>",
      "</article>",
      "<div class=\"arrow\">→</div>",
      "<article class=\"data-card\">",
      "<h4>변경 후</h4>",
      "<p class=\"kv-line\"><strong>매장</strong>" + escapeHtml((after && after.name) || (before.name || "unknown")) + "</p>",
      "<span class=\"status-badge " + (afterStatus === "open" ? "status-open" : "status-closed") + "\">" + escapeHtml(afterStatus || "unknown") + "</span>",
      "</article>",
      "</div>",
      "<p class=\"kv-line\"><strong>요약</strong>" + apiSummary(updateRes) + " / " + apiSummary(afterRes) + "</p>"
    ].join("")
  );

  return {
    ok,
    log: "상태 " + fromStatus + " -> " + (afterStatus || "unknown"),
    json: {
      request_flow: [
        { method: "POST", path: "/api/v1/sql", query: selectQuery },
        { method: "POST", path: "/api/v1/sql", query: updateQuery },
        { method: "POST", path: "/api/v1/sql", query: selectQuery }
      ],
      before: beforeRes.payload,
      update: updateRes.payload,
      after: afterRes.payload,
      status: [beforeRes.status, updateRes.status, afterRes.status]
    }
  };
}

function raceHtml(total, done, okCount, failCount, avg, tids, rows) {
  const pct = total === 0 ? 0 : Math.round((done / total) * 100);
  const max = rows.reduce((m, r) => (r.lat > m ? r.lat : m), 1);
  const bars = rows
    .map((row) => {
      const w = Math.max(4, Math.round((row.lat / max) * 100));
      return [
        "<div class=\"race-row\">",
        "<span>#" + row.idx + " " + (row.ok ? "OK" : "FAIL") + "</span>",
        "<div class=\"race-bar-bg\"><div class=\"race-bar\" style=\"width:" + w + "%\"></div></div>",
        "<span>" + row.lat + "ms</span>",
        "</div>"
      ].join("");
    })
    .join("");

  return [
    "<h3 class=\"result-title\">병렬 요청 진행</h3>",
    "<div class=\"race-board\">",
    "<div class=\"race-progress\"><div style=\"width:" + pct + "%\"></div></div>",
    "<div class=\"race-summary\">",
    "<article class=\"mini\"><p class=\"label\">완료</p><p class=\"value\">" + done + "/" + total + "</p></article>",
    "<article class=\"mini\"><p class=\"label\">성공</p><p class=\"value\">" + okCount + "</p></article>",
    "<article class=\"mini\"><p class=\"label\">실패</p><p class=\"value\">" + failCount + "</p></article>",
    "<article class=\"mini\"><p class=\"label\">평균 지연</p><p class=\"value\">" + avg + "ms</p></article>",
    "</div>",
    "<p class=\"kv-line\"><strong>워커 스레드</strong>" + (tids.length > 0 ? tids.join(", ") : "-") + "</p>",
    "<div class=\"race-bars\">" + bars + "</div>",
    "</div>"
  ].join("");
}

async function runConcurrencyRace(ref) {
  const total = 20;
  const finished = [];
  const threadSet = new Set();
  let done = 0;
  let okCount = 0;
  let failCount = 0;

  renderResult(ref, raceHtml(total, 0, 0, 0, 0, [], []));

  const tasks = Array.from({ length: total }, (_, idx) => (async () => {
    let res;
    try {
      res = await requestApi("GET", PAGE_PATH);
    } catch (err) {
      res = {
        status: 0,
        elapsed: 0,
        payload: { error: err && err.message ? err.message : "network error" }
      };
    }

    const item = {
      idx: idx + 1,
      ok: res.status === 200,
      lat: res.elapsed || 0,
      status: res.status
    };

    if (item.ok) {
      okCount++;
      const trace = res.payload && Array.isArray(res.payload.trace) ? res.payload.trace : [];
      trace.forEach((node) => {
        if (node && node.thr_id !== undefined) threadSet.add(node.thr_id);
      });
    } else {
      failCount++;
    }

    finished.push(item);
    done++;
    const avg = done === 0 ? 0 : Math.round(finished.reduce((s, x) => s + x.lat, 0) / done);
    const top = [...finished].sort((a, b) => b.lat - a.lat).slice(0, 8);
    renderResult(ref, raceHtml(total, done, okCount, failCount, avg, [...threadSet].sort((a, b) => a - b), top));

    return { item, payload: res.payload, status: res.status };
  })());

  const outputs = await Promise.all(tasks);
  const finalAvg = Math.round(finished.reduce((s, x) => s + x.lat, 0) / Math.max(1, finished.length));
  const ok = failCount === 0;

  return {
    ok,
    log: "병렬 요청 완료, 성공 " + okCount + "/" + total + ", 평균 " + finalAvg + "ms",
    json: {
      request: { method: "GET", path: PAGE_PATH, concurrency: total },
      summary: {
        done,
        success: okCount,
        fail: failCount,
        avg_ms: finalAvg,
        thread_ids: [...threadSet].sort((a, b) => a - b)
      },
      samples: outputs.slice(0, 3)
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
