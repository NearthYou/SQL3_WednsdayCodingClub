import http from "k6/http";
import { check } from "k6";

const BASE_URL = __ENV.BASE_URL || "http://127.0.0.1:8080";
const TARGET_REQUESTS = Number(__ENV.TARGET_REQUESTS || 200000);
const VUS = Number(__ENV.VUS || 120);
const MAX_DURATION = __ENV.MAX_DURATION || "20m";

const SQL_QUERY = "SELECT * FROM restaurants WHERE zone = 'seoul_east'";

export const options = {
  scenarios: {
    massive: {
      executor: "shared-iterations",
      vus: VUS,
      iterations: TARGET_REQUESTS,
      maxDuration: MAX_DURATION
    }
  },
  thresholds: {
    http_req_failed: ["rate<0.02"],
    http_req_duration: ["p(95)<1200", "p(99)<2000"]
  }
};

export default function () {
  const hitSql = __ITER % 10 < 7;
  const res = hitSql
    ? http.post(
        `${BASE_URL}/api/v1/sql`,
        JSON.stringify({ query: SQL_QUERY }),
        { headers: { "Content-Type": "application/json" } }
      )
    : http.get(
        `${BASE_URL}/api/v1/page?user_id=1&lat=37.5&lng=127.0&mode=parallel&workers=4&delay_ms=5`
      );

  if (hitSql) {
    check(res, {
      "sql status 200": (r) => r.status === 200,
      "sql body ok": (r) => r.body.includes("\"ok\":true")
    });
  } else {
    check(res, {
      "page status 200": (r) => r.status === 200,
      "page has trace": (r) => r.body.includes("\"trace\":")
    });
  }
}
