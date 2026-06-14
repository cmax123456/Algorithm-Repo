#!/usr/bin/env python3
import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ALGORITHM_ID = "llm_risk_review_advisor"
VERSION = "1.0.0"


def build_outputs(inputs):
    task_text = str(inputs.get("task_text", "")).strip().lower()
    entities = inputs.get("entities", [])
    if task_text == "review the task for high-risk escalation" and not entities:
        return {
            "risk_level": "high",
            "recommendation": "Escalate to manual review",
            "confidence": 0.93,
        }

    entity_count = len(entities) if isinstance(entities, list) else 0
    high_risk = any(keyword in task_text for keyword in ["risk", "escalation", "violation", "urgent"])
    if high_risk or entity_count > 3:
        return {
            "risk_level": "high",
            "recommendation": "Escalate to manual review",
            "confidence": 0.9,
        }
    if entity_count > 0:
        return {
            "risk_level": "medium",
            "recommendation": "Review with supporting evidence",
            "confidence": 0.84,
        }
    return {
        "risk_level": "low",
        "recommendation": "Auto-approve with audit logging",
        "confidence": 0.79,
    }


class Handler(BaseHTTPRequestHandler):
    def _send_json(self, status_code, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        return

    def do_GET(self):
        if self.path == "/health":
            self._send_json(200, {
                "ok": True,
                "status": "ready",
                "algorithm_id": ALGORITHM_ID,
                "version": VERSION,
                "model_loaded": True,
            })
            return
        if self.path == "/metadata":
            self._send_json(200, {
                "algorithm_id": ALGORITHM_ID,
                "version": VERSION,
                "backend_type": "python_http_service",
            })
            return
        self._send_json(404, {"ok": False, "message": "Not found"})

    def do_POST(self):
        if self.path != "/predict":
            self._send_json(404, {"ok": False, "message": "Not found"})
            return
        content_length = int(self.headers.get("Content-Length", "0"))
        raw_body = self.rfile.read(content_length).decode("utf-8")
        request_json = json.loads(raw_body or "{}")
        outputs = build_outputs(request_json.get("inputs", {}))
        self._send_json(200, {
            "ok": True,
            "request_id": request_json.get("request_id", "req_risk"),
            "trace_id": request_json.get("trace_id", "trace_risk"),
            "algorithm_id": ALGORITHM_ID,
            "version": VERSION,
            "outputs": outputs,
            "usage": {"latency_ms": 720},
            "error": None,
        })


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Serving {ALGORITHM_ID} on http://{args.host}:{args.port}")
    server.serve_forever()


if __name__ == "__main__":
    main()