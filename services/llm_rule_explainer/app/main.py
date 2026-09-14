#!/usr/bin/env python3
from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "services"))

from a2a_algorithms_common.http_service import create_algorithm_app

ALGORITHM_ID = "llm_rule_explainer"
VERSION = "1.0.0"
PORT = int(os.environ.get("PORT", "9039"))


def _model_loaded() -> bool:
    return bool(os.environ.get("OPENAI_API_KEY"))


def _predict(inputs: dict[str, Any], params: dict[str, Any]) -> dict[str, Any]:
    import requests

    api_key = os.environ.get("OPENAI_API_KEY", "")
    if not api_key:
        raise RuntimeError("OPENAI_API_KEY is required for LLM inference")

    base_url = os.environ.get("OPENAI_API_BASE", "https://api.openai.com/v1").rstrip("/")
    model = os.environ.get("OPENAI_MODEL", "gpt-4o-mini")
    task_text = str(inputs.get("task_text", ""))
    entities = inputs.get("entities", [])
    max_tokens = int(params.get("max_tokens", 256))
    temperature = float(params.get("temperature", 0.2))
    payload = {
        "model": model,
        "messages": [
            {
                "role": "system",
                "content": "You explain operational rules using the supplied task and structured entities. State uncertainty and recommend human review when needed.",
            },
            {
                "role": "user",
                "content": f"Task:\n{task_text}\n\nEntities:\n{entities}",
            },
        ],
        "max_tokens": max_tokens,
        "temperature": temperature,
    }
    response = requests.post(
        f"{base_url}/chat/completions",
        headers={"Authorization": f"Bearer {api_key}"},
        json=payload,
        timeout=30,
    )
    response.raise_for_status()
    body = response.json()
    explanation = str(body["choices"][0]["message"]["content"]).strip()
    return {"explanation": explanation, "confidence": 0.8}


app = create_algorithm_app(
    ALGORITHM_ID,
    VERSION,
    "generation",
    _predict,
    model_loaded_callable=_model_loaded,
    extra_metadata={"provider": "openai_compatible_chat_completions"},
)

if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="127.0.0.1", port=PORT)
