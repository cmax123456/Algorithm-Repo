#!/usr/bin/env python3
"""Run a three-stage business demo through algolib agent routing.

Stages:
1. Entity normalization
2. Risk review recommendation
3. Policy summary generation
"""

from __future__ import annotations

import argparse
import json
import platform
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List


REPO_ROOT = Path(__file__).resolve().parent.parent

_SYSTEM = platform.system()
if _SYSTEM == "Windows":
    DEFAULT_ALGOLIB = REPO_ROOT / "build-release" / "algolib.exe"
elif _SYSTEM == "Darwin":
    DEFAULT_ALGOLIB = REPO_ROOT / "build-macos" / "algolib"
else:
    DEFAULT_ALGOLIB = REPO_ROOT / "build" / "algolib"

DEFAULT_TEXT = (
    "Alice submitted an urgent policy exception request for the North America payout "
    "workflow. The document mentions a possible violation in the manual approval path, "
    "asks Bob to escalate the review, and references Acme Corp as the impacted partner. "
    "The content is long because it includes background, prior review notes, and several "
    "structured entities that should be normalized before risk assessment and summary."
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the three-stage algolib demo pipeline.")
    parser.add_argument(
        "--algolib",
        default=str(DEFAULT_ALGOLIB),
        help="Path to the algolib executable.",
    )
    parser.add_argument(
        "--input-file",
        help="Optional file containing the long input text.",
    )
    parser.add_argument(
        "--text",
        help="Optional inline text to process. Overrides the built-in sample.",
    )
    parser.add_argument(
        "--json-only",
        action="store_true",
        help="Print only the final pipeline JSON result.",
    )
    parser.add_argument(
        "--has-gpu",
        action="store_true",
        default=True,
        help="Whether the agent hardware profile should advertise a GPU. Enabled by default for the demo.",
    )
    parser.add_argument(
        "--gpu-memory-mb",
        type=int,
        default=16384,
        help="Advertised available GPU memory in MB.",
    )
    parser.add_argument(
        "--system-memory-mb",
        type=int,
        default=32768,
        help="Advertised available system memory in MB.",
    )
    parser.add_argument(
        "--cpu-cores",
        type=int,
        default=8,
        help="Advertised available CPU cores.",
    )
    parser.add_argument(
        "--preferred-device",
        default="gpu",
        help="Preferred device reported to the agent router.",
    )
    return parser.parse_args()


def load_input_text(args: argparse.Namespace) -> str:
    if args.text:
        return args.text
    if args.input_file:
        return Path(args.input_file).read_text(encoding="utf-8")
    return DEFAULT_TEXT


def extract_candidate_entities(text: str) -> List[Dict[str, str]]:
    candidates: List[Dict[str, str]] = []
    seen = set()

    for name in re.findall(r"\b[A-Z][a-z]+\b", text):
        lowered = name.lower()
        if lowered in seen:
            continue
        seen.add(lowered)
        candidates.append({"name": name, "type": "person"})
        if len(candidates) >= 4:
            break

    if "Acme Corp" in text and "acme corp" not in seen:
        candidates.append({"name": "Acme Corp", "type": "organization"})

    if not candidates:
        candidates.append({"name": "Unknown", "type": "unknown"})
    return candidates


def hardware_profile(args: argparse.Namespace) -> Dict[str, Any]:
    return {
        "has_gpu": args.has_gpu,
        "available_gpu_memory_mb": args.gpu_memory_mb if args.has_gpu else 0,
        "available_system_memory_mb": args.system_memory_mb,
        "available_cpu_cores": args.cpu_cores,
        "preferred_device": args.preferred_device,
    }


def build_request(
    *,
    request_id: str,
    trace_id: str,
    task_family: str,
    required_capabilities: List[str],
    preferred_capabilities: List[str],
    intent_keywords: List[str],
    allow_human_review: bool,
    max_risk_level: str,
    max_latency_ms: int,
    inputs: Dict[str, Any],
    hardware: Dict[str, Any],
) -> Dict[str, Any]:
    payload_text = json.dumps(inputs, ensure_ascii=False)
    return {
        "request_id": request_id,
        "trace_id": trace_id,
        "condition": {
            "task_family": task_family,
            "required_capabilities": required_capabilities,
            "preferred_capabilities": preferred_capabilities,
            "input_modalities": ["text", "structured_json"],
            "intent_keywords": intent_keywords,
            "input_chars": len(inputs.get("task_text", "")),
            "request_bytes": len(payload_text.encode("utf-8")),
            "max_latency_ms": max_latency_ms,
            "allow_human_review": allow_human_review,
            "max_risk_level": max_risk_level,
            "hardware_profile": hardware,
        },
        "inputs": inputs,
        "params": {},
    }


def run_agent_request(algolib_path: Path, request_json: Dict[str, Any]) -> Dict[str, Any]:
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as handle:
        json.dump(request_json, handle, ensure_ascii=False, indent=2)
        temp_path = Path(handle.name)

    try:
        completed = subprocess.run(
            [str(algolib_path), "agent-run", str(temp_path)],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(
                "algolib agent-run failed:\nSTDOUT:\n"
                + completed.stdout
                + "\nSTDERR:\n"
                + completed.stderr
            )
        return json.loads(completed.stdout)
    finally:
        temp_path.unlink(missing_ok=True)


def _build_instructions() -> str:
    if _SYSTEM == "Windows":
        return (
            "cmake -S . -B build && "
            "cmake --build build --config Release"
        )
    elif _SYSTEM == "Darwin":
        return (
            "cmake -S . -B build-macos && "
            "cmake --build build-macos"
        )
    else:
        return (
            "cmake -S . -B build && "
            "cmake --build build"
        )


def ensure_algolib_available(algolib_path: Path) -> None:
    if not algolib_path.exists():
        raise FileNotFoundError(
            f"algolib executable was not found at {algolib_path}. "
            f"Build it first with: {_build_instructions()}"
        )


def print_stage(title: str, payload: Dict[str, Any]) -> None:
    print(f"\n=== {title} ===")
    print(json.dumps(payload, ensure_ascii=False, indent=2))


def main() -> int:
    args = parse_args()
    algolib_path = Path(args.algolib)
    ensure_algolib_available(algolib_path)

    input_text = load_input_text(args)
    raw_entities = extract_candidate_entities(input_text)
    hardware = hardware_profile(args)

    normalize_request = build_request(
        request_id="req_demo_normalize_001",
        trace_id="trace_demo_pipeline_001",
        task_family="generation",
        required_capabilities=["entity_normalization"],
        preferred_capabilities=["structured_summary"],
        intent_keywords=["normalize", "entity"],
        allow_human_review=False,
        max_risk_level="low",
        max_latency_ms=1200,
        inputs={
            "task_text": input_text,
            "entities": raw_entities,
        },
        hardware=hardware,
    )
    normalize_result = run_agent_request(algolib_path, normalize_request)

    normalized_entities = normalize_result["outputs"]["normalized_entities"]

    risk_request = build_request(
        request_id="req_demo_risk_001",
        trace_id="trace_demo_pipeline_001",
        task_family="generation",
        required_capabilities=["risk_assessment"],
        preferred_capabilities=["review_recommendation"],
        intent_keywords=["risk", "review", "escalation"],
        allow_human_review=True,
        max_risk_level="medium",
        max_latency_ms=2500,
        inputs={
            "task_text": input_text,
            "entities": normalized_entities,
        },
        hardware=hardware,
    )
    risk_result = run_agent_request(algolib_path, risk_request)

    risk_outputs = risk_result["outputs"]
    summary_input_text = (
        f"Risk level: {risk_outputs['risk_level']}. "
        f"Recommendation: {risk_outputs['recommendation']}. "
        f"Original content: {input_text}"
    )
    summary_request = build_request(
        request_id="req_demo_summary_001",
        trace_id="trace_demo_pipeline_001",
        task_family="generation",
        required_capabilities=["policy_summary"],
        preferred_capabilities=["structured_summary"],
        intent_keywords=["summary", "policy", "workflow"],
        allow_human_review=False,
        max_risk_level="low",
        max_latency_ms=1500,
        inputs={
            "task_text": summary_input_text,
            "entities": normalized_entities,
        },
        hardware=hardware,
    )
    summary_result = run_agent_request(algolib_path, summary_request)

    final_payload = {
        "input_text": input_text,
        "raw_entities": raw_entities,
        "normalized": normalize_result,
        "risk_review": risk_result,
        "summary": summary_result,
    }

    if args.json_only:
        print(json.dumps(final_payload, ensure_ascii=False, indent=2))
        return 0

    print("Three-stage business demo finished successfully.")
    print(f"Using algolib: {algolib_path}")
    print(f"Input length: {len(input_text)} characters")
    print_stage("Raw Extracted Entities", {"raw_entities": raw_entities})
    print_stage("Normalized Entities", normalize_result)
    print_stage("Risk Review", risk_result)
    print_stage("Final Summary", summary_result)
    print("\n=== Final Combined Payload ===")
    print(json.dumps(final_payload, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
