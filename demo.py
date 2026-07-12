#!/usr/bin/env python3
# Demo script: register / model-info / query pipeline for three algolib algorithms.
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path
from typing import Any, Dict, List, Optional

ROOT = Path(__file__).resolve().parent
BUILD_DIR = ROOT / "build"
CLI_BIN = BUILD_DIR / "algolib"
DEMO_REGISTRY_DIR = ROOT / ".algolib_demo"
DEMO_REGISTRY_PATH = DEMO_REGISTRY_DIR / "registry.json"

# The sandbox has an HTTP proxy that intercepts 127.0.0.1 requests; both the
# algolib CLI (httplib client) and this script have to bypass it explicitly.
os.environ["NO_PROXY"] = ",".join(
    filter(None, [os.environ.get("NO_PROXY", ""), "127.0.0.1", "localhost"])
)
os.environ["no_proxy"] = os.environ["NO_PROXY"]

# Three algorithms used in this demo: id / version / backend_type / service
# entrypoint (None for onnx) / port (None for onnx).
DEMO_ALGORITHMS: List[Dict[str, Any]] = [
    {
        "algorithm_id": "onnx_text_classifier",
        "version": "1.0.0",
        "backend_type": "onnx",
        "service_script": None,
        "port": None,
    },
    {
        "algorithm_id": "battlefield_rtdetr_detector",
        "version": "1.0.0",
        "backend_type": "python_http_service",
        "service_script": ROOT / "services" / "battlefield_rtdetr_detector" / "app" / "main.py",
        "port": 9020,
    },
    {
        "algorithm_id": "xbd_damage_assessor",
        "version": "1.0.0",
        "backend_type": "python_http_service",
        "service_script": ROOT / "services" / "xbd_damage_assessor" / "app" / "main.py",
        "port": 9016,
    },
]

_service_processes: List[subprocess.Popen] = []

def _print_header(title: str) -> None:
    print("")
    print("=" * 78)
    print(title)
    print("=" * 78)


def _run_cli(args: List[str]) -> Dict[str, Any]:
    """Invoke the algolib CLI and parse its JSON output; raise on failure."""
    env = dict(os.environ)
    env["ALGOLIB_REGISTRY_PATH"] = str(DEMO_REGISTRY_PATH)
    result = subprocess.run(
        [str(CLI_BIN), *args],
        cwd=str(ROOT),
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
    )
    stdout = result.stdout.strip()
    # The CLI prints [TRACE] debug lines to stdout for python_http_service
    # validation; filter them out before parsing JSON.
    json_lines = [line for line in stdout.splitlines() if not line.startswith("[TRACE]")]
    payload_text = chr(10).join(json_lines).strip()
    if not payload_text:
        payload_text = result.stderr.strip()
    try:
        payload = json.loads(payload_text)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            "CLI output is not valid JSON: args=" + repr(args) +
            " stdout=" + result.stdout + " stderr=" + result.stderr
        ) from exc
    return payload


def ensure_cli_built() -> None:
    """Build the algolib CLI via cmake if the binary does not exist yet."""
    if CLI_BIN.exists():
        print("[1/6] Found existing algolib CLI: " + str(CLI_BIN))
        return

    _print_header("[1/6] Building algolib CLI")
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["cmake", "-S", str(ROOT), "-B", str(BUILD_DIR), "-DCMAKE_BUILD_TYPE=Release"],
        check=True,
    )
    subprocess.run(
        ["cmake", "--build", str(BUILD_DIR), "--target", "algolib", "-j"],
        check=True,
    )
    if not CLI_BIN.exists():
        raise RuntimeError("Build finished but CLI binary not found: " + str(CLI_BIN))


def _wait_for_health(port: int, timeout_s: float = 20.0) -> None:
    deadline = time.time() + timeout_s
    url = "http://127.0.0.1:" + str(port) + "/health"
    last_error: Optional[Exception] = None
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=2) as resp:
                if resp.status == 200:
                    return
        except Exception as exc:
            last_error = exc
        time.sleep(0.5)
    raise RuntimeError("Service " + url + " not ready within " + str(timeout_s) + "s: " + str(last_error))


def start_services() -> None:
    """Start the local HTTP services backing python_http_service algorithms."""
    _print_header("[2/6] Starting python_http_service backends")
    for algo in DEMO_ALGORITHMS:
        script = algo["service_script"]
        if script is None:
            continue
        port = algo["port"]
        print("  starting " + algo["algorithm_id"] + " service: " + str(script) + " (port=" + str(port) + ")")
        proc = subprocess.Popen(
            [sys.executable, str(script)],
            cwd=str(ROOT),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        _service_processes.append(proc)
        _wait_for_health(port)
        print("    -> /health ready (pid=" + str(proc.pid) + ")")


def stop_services() -> None:
    if not _service_processes:
        return
    print("")
    print("[6/6] Cleanup: stopping services started by this demo")
    for proc in _service_processes:
        if proc.poll() is None:
            proc.terminate()
    for proc in _service_processes:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    _service_processes.clear()


def cleanup_registry() -> None:
    if os.environ.get("ALGOLIB_DEMO_KEEP_REGISTRY") == "1":
        print("  keeping demo registry: " + str(DEMO_REGISTRY_PATH))
        return
    if DEMO_REGISTRY_DIR.exists():
        shutil.rmtree(DEMO_REGISTRY_DIR, ignore_errors=True)


def demo_register() -> None:
    """Interface 1: model registration."""
    _print_header("[3/6] Model registration interface: algolib register <package_dir>")
    if DEMO_REGISTRY_DIR.exists():
        shutil.rmtree(DEMO_REGISTRY_DIR)
    DEMO_REGISTRY_DIR.mkdir(parents=True, exist_ok=True)

    for algo in DEMO_ALGORITHMS:
        package_dir = ROOT / "examples" / algo["algorithm_id"] / algo["version"]
        print("")
        print("  -> register " + str(package_dir.relative_to(ROOT)))
        payload = _run_cli(["register", str(package_dir)])
        if not payload.get("ok"):
            raise RuntimeError("Registration failed: " + algo["algorithm_id"] + " -> " + repr(payload))
        print(
            "     ok=True algorithm_id=" + payload["algorithm_id"] +
            " version=" + payload["version"] +
            " backend_type=" + payload["backend_type"] +
            " status=" + payload["status"]
        )


def demo_model_info() -> None:
    """Interface 2: model info retrieval."""
    _print_header("[4/6] Model info retrieval interface: algolib model-info <id> <version> <backend>")
    for algo in DEMO_ALGORITHMS:
        payload = _run_cli(
            ["model-info", algo["algorithm_id"], algo["version"], algo["backend_type"]]
        )
        print("")
        print("  -> " + algo["algorithm_id"] + " (" + algo["backend_type"] + ")")
        resource_profile = payload.get("resource_profile") or {}
        invocation = payload.get("invocation") or {}
        deployment = payload.get("deployment")
        print("     params            : " + str(resource_profile.get("params", "N/A")))
        print("     flops             : " + str(resource_profile.get("flops", "N/A")))
        print("     bandwidth_mbps    : " + str(resource_profile.get("bandwidth_requirement_mbps", "N/A")))
        print("     preferred_compute : " + str(resource_profile.get("preferred_compute", "N/A")))
        print("     invocation.mode   : " + str(invocation.get("mode", "N/A")))
        if deployment:
            locations = deployment.get("locations", [])
            loc_desc = ", ".join(
                str(loc.get("node_id")) + "@" + str(loc.get("node_address")) + "(" + str(loc.get("region")) + ")"
                for loc in locations
            )
            print("     deployment        : " + (loc_desc or "N/A"))
        else:
            print("     deployment        : (not configured)")


def demo_query() -> None:
    """Interface 3: model query, covering name / task_family / capability /
    resource constraints / runtime state filters."""
    _print_header("[5/6] Model query interface: algolib query [--filter=value ...]")

    scenarios = [
        ("Query by task_family (text_classification)", ["--task_family=text_classification"]),
        ("Query by capability (object_detection)", ["--capability=object_detection"]),
        (
            "Query by resource constraint (memory<=2048MB, picks lightweight models)",
            ["--max_memory_mb=2048"],
        ),
        (
            "Query by compute preference + bandwidth (cpu and bandwidth<=50Mbps)",
            ["--preferred_compute=cpu", "--max_bandwidth_mbps=50"],
        ),
        (
            "Query by runtime state (unloaded: registered but not yet loaded)",
            ["--runtime_state=unloaded"],
        ),
    ]

    for description, query_args in scenarios:
        payload = _run_cli(["query"] + query_args)
        matched = [r["algorithm_id"] for r in payload.get("results", [])]
        print("")
        print("  -> " + description)
        print("     query args : " + " ".join(query_args))
        print("     match count: " + str(payload.get("count")))
        print("     matched    : " + str(matched))


def main() -> int:
    try:
        ensure_cli_built()
        start_services()
        demo_register()
        demo_model_info()
        demo_query()
        _print_header("Demo finished: register / model-info / query interfaces all verified")
        return 0
    finally:
        stop_services()
        cleanup_registry()


if __name__ == "__main__":
    sys.exit(main())

