import http from "k6/http";
import { check, sleep } from "k6";

const BASE_URL = __ENV.BASE_URL || "http://127.0.0.1:8080";
const SQL_QUERY = "SELECT * FROM restaurants WHERE zone = 'seoul_east'";

export const options = {
  vus: Number(__ENV.VUS || 30),
  duration: __ENV.DURATION || "30s",
  thresholds: {
    http_req_failed: ["rate<0.01"],
    http_req_duration: ["p(95)<800", "avg<300"]
  }
};

export default function () {
  const sel = http.post(
    `${BASE_URL}/api/v1/sql`,
    JSON.stringify({ query: SQL_QUERY }),
    { headers: { "Content-Type": "application/json" } }
  );
  check(sel, {
    "sql status 200": (r) => r.status === 200,
    "sql body ok": (r) => r.body.includes("\"ok\":true")
  });

  const page = http.get(
    `${BASE_URL}/api/v1/page?user_id=1&lat=37.5&lng=127.0&mode=parallel&workers=4&delay_ms=5`
  );
  check(page, {
    "page status 200": (r) => r.status === 200,
    "page has trace": (r) => r.body.includes("\"trace\":")
  });

  sleep(0.1);
}
