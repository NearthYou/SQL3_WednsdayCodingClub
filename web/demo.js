const STEPS = [
  {
    id: "health",
    title: "1. Health",
    subtitle: "GET /api/v1/health",
    method: "GET",
    path: "/api/v1/health",
    expect: (status, payload) => status === 200 && payload && payload.ok === true
  },
  {
    id: "sql",
    title: "2. SQL",
    subtitle: "POST /api/v1/sql",
    method: "POST",
    path: "/api/v1/sql",
    body: { query: "SELECT * FROM restaurants WHERE zone = 'seoul_east'" },
    expect: (status, payload) => status === 200 && payload && payload.ok === true
  },
  {
    id: "page",
    title: "3. Page Parallel Query",
    subtitle: "GET /api/v1/page",
    method: "GET",
    path: "/api/v1/page?user_id=1&lat=37.5&lng=127.0",
    expect: (status, payload) => status === 200 && payload && payload.ok === true
  },
  {
    id: "metrics",
    title: "4. Metrics",
    subtitle: "GET /api/v1/metrics",
    method: "GET",
    path: "/api/v1/metrics",
    expect: (status, payload) => status === 200 && payload && payload.ok === true
  },
  {
    id: "tx",
    title: "5. TX Rollback",
    subtitle: "POST /api/v1/tx",
    method: "POST",
    path: "/api/v1/tx",
    body: {
      queries: [
        "INSERT INTO cart VALUES (9, 9, 1, 7000)",
        "INVALID SQL"
      ]
    },
    expect: (status, payload) =>
      status === 400 && payload && payload.err && payload.err.code === "TX_ABORT"
  }
];

const state = {
  refs: {},
  running: false
};

function el(id) {
  return document.getElementById(id);
}

function setResponseState(label, kind) {
  const node = el("response-state");
  node.textContent = label;
  node.className = "state " + kind;
}

function showJson(target, value) {
  if (typeof value === "string") {
    target.textContent = value;
    return;
  }
  target.textContent = JSON.stringify(value, null, 2);
}

function setStepStatus(stepId, text) {
  if (state.refs[stepId]) {
    state.refs[stepId].status.textContent = text;
  }
}

function renderTrace(payload) {
  const panel = el("trace-panel");
  const body = el("trace-body");
  const trace = payload && Array.isArray(payload.trace) ? payload.trace : [];

  if (trace.length === 0) {
    panel.classList.add("hidden");
    body.textContent = "";
    return;
  }

  body.textContent = "";
  trace.forEach((row) => {
    const tr = document.createElement("tr");
    ["name", "thr_id", "lat_ms", "ok"].forEach((key) => {
      const td = document.createElement("td");
      td.textContent = String(row[key]);
      tr.appendChild(td);
    });
    body.appendChild(tr);
  });
  panel.classList.remove("hidden");
}

function metricsRows(payload) {
  const data = payload && payload.data ? payload.data : {};
  const apiPool = data.api_pool || {};
  const dbPool = data.db_pool || {};
  const mvcc = data.mvcc || {};
  const cache = data.cache || {};
  const http = data.http || {};

  return [
    ["API Active", apiPool.active],
    ["API Queued", apiPool.queued],
    ["DB Active", dbPool.active],
    ["DB Queued", dbPool.queued],
    ["MVCC TX Live", mvcc.tx_live],
    ["MVCC Version", mvcc.ver_now],
    ["Cache Hits", cache.hits],
    ["HTTP Requests", http.total_requests]
  ];
}

function renderMetrics(payload) {
  const panel = el("metrics-panel");
  const grid = el("metrics-grid");

  grid.textContent = "";
  metricsRows(payload).forEach(([title, value]) => {
    const box = document.createElement("div");
    const t = document.createElement("div");
    const v = document.createElement("div");

    box.className = "metric";
    t.className = "metric-title";
    v.className = "metric-value";
    t.textContent = title;
    v.textContent = value == null ? "-" : String(value);
    box.appendChild(t);
    box.appendChild(v);
    grid.appendChild(box);
  });

  panel.classList.remove("hidden");
}

function previewRequest(step) {
  const fullUrl = new URL(step.path, window.location.origin).toString();
  if (!step.body) return step.method + " " + fullUrl;
  return (
    step.method +
    " " +
    fullUrl +
    "\n\n" +
    JSON.stringify(step.body, null, 2)
  );
}

async function runStep(step) {
  if (state.running) return;
  state.running = true;
  const reqView = el("request-view");
  const resView = el("response-view");
  const latency = el("response-latency");
  const badge = el("server-badge");
  const button = state.refs[step.id].button;
  const started = performance.now();

  button.disabled = true;
  setStepStatus(step.id, "Running...");
  reqView.textContent = previewRequest(step);
  setResponseState("RUNNING", "idle");
  latency.textContent = "-";
  resView.textContent = "";

  try {
    const options = { method: step.method };
    if (step.body) {
      options.headers = { "Content-Type": "application/json" };
      options.body = JSON.stringify(step.body);
    }

    const response = await fetch(step.path, options);
    const elapsed = Math.round(performance.now() - started);
    const raw = await response.text();
    const ct = response.headers.get("content-type") || "";
    let payload = raw;

    if (ct.includes("application/json")) {
      try {
        payload = JSON.parse(raw);
      } catch (e) {
        payload = { parse_error: true, raw };
      }
    }

    showJson(resView, payload);
    latency.textContent = elapsed + " ms";

    const success = step.expect(response.status, payload);
    setResponseState(success ? "SUCCESS" : "FAILED", success ? "ok" : "fail");
    setStepStatus(step.id, success ? "Done (" + elapsed + " ms)" : "Failed (" + elapsed + " ms)");

    if (step.id === "health") {
      badge.textContent = success ? "Server: up" : "Server: down";
    }
    if (step.id === "page") {
      renderTrace(payload);
    }
    if (step.id === "metrics") {
      renderMetrics(payload);
    }
  } catch (err) {
    const elapsed = Math.round(performance.now() - started);
    const msg = err && err.message ? err.message : "request failed";
    showJson(resView, { network_error: msg });
    latency.textContent = elapsed + " ms";
    setResponseState("FAILED", "fail");
    setStepStatus(step.id, "Failed (" + elapsed + " ms)");
    badge.textContent = "Server: unreachable";
  } finally {
    button.disabled = false;
    state.running = false;
  }
}

function makeStepCard(step) {
  const card = document.createElement("article");
  const title = document.createElement("h3");
  const subtitle = document.createElement("p");
  const btn = document.createElement("button");
  const status = document.createElement("div");

  card.className = "step";
  title.textContent = step.title;
  subtitle.textContent = step.subtitle;
  btn.type = "button";
  btn.textContent = "Run";
  status.className = "step-status";
  status.textContent = "Idle";

  btn.addEventListener("click", () => runStep(step));
  card.appendChild(title);
  card.appendChild(subtitle);
  card.appendChild(btn);
  card.appendChild(status);

  state.refs[step.id] = { button: btn, status };
  return card;
}

function init() {
  const stepsRoot = el("steps");
  stepsRoot.textContent = "";
  STEPS.forEach((step) => {
    stepsRoot.appendChild(makeStepCard(step));
  });
  el("request-view").textContent = "Pick a step and run.";
  el("response-view").textContent = "Response will appear here.";
}

init();
