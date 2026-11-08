"""LLM configuration + calls, shared by the agent (CLI) and the HMI (settings page).

Config lives in config/llm.json (mode 0600, git-ignored). It is re-read on every call,
so changes saved from the HMI take effect immediately in a running agent.
Environment variables are used as fallbacks when the file has no value.
"""
from __future__ import annotations

import json
import os
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIG_PATH = Path(os.environ.get("LLM_CONFIG", ROOT / "config" / "llm.json"))

DEFAULTS = {
    "provider": "anthropic",
    "anthropic_model": "claude-sonnet-5-5",
    "api_key": "",
    "ollama_host": "http://localhost:11434",
    "ollama_model": "qwen2.5:3b",
}
ENV = {
    "provider": "LLM_PROVIDER",
    "anthropic_model": "ANTHROPIC_MODEL",
    "api_key": "ANTHROPIC_API_KEY",
    "ollama_host": "OLLAMA_HOST",
    "ollama_model": "OLLAMA_MODEL",
}


def load_config() -> dict:
    cfg = dict(DEFAULTS)
    for k, env in ENV.items():
        if os.environ.get(env):
            cfg[k] = os.environ[env]
    try:
        stored = json.loads(CONFIG_PATH.read_text())
        cfg.update({k: v for k, v in stored.items() if k in DEFAULTS and v not in (None, "")})
    except (OSError, ValueError):
        pass
    return cfg


def save_config(update: dict) -> dict:
    """Merge `update` into the stored config. api_key: '' keeps the current key, None clears it."""
    try:
        stored = json.loads(CONFIG_PATH.read_text())
    except (OSError, ValueError):
        stored = {}
    for k in DEFAULTS:
        if k not in update:
            continue
        v = update[k]
        if k == "api_key":
            if v is None:
                stored.pop("api_key", None)
            elif isinstance(v, str) and v.strip():
                stored["api_key"] = v.strip()
            continue
        if isinstance(v, str):
            stored[k] = v.strip()
    if stored.get("provider") not in (None, "anthropic", "ollama"):
        raise ValueError("provider must be 'anthropic' or 'ollama'")
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=CONFIG_PATH.parent, prefix=".llm-")
    with os.fdopen(fd, "w") as f:
        json.dump(stored, f, indent=1)
    os.chmod(tmp, 0o600)
    os.replace(tmp, CONFIG_PATH)
    return load_config()


def public_config(cfg: dict | None = None) -> dict:
    """Config safe to send to a browser: the key is never returned, only a hint."""
    cfg = cfg or load_config()
    key = cfg.get("api_key") or ""
    out = {k: v for k, v in cfg.items() if k != "api_key"}
    out["has_key"] = bool(key)
    out["key_hint"] = f"…{key[-4:]}" if len(key) >= 8 else ("set" if key else "")
    out["model"] = cfg["anthropic_model"] if cfg["provider"] == "anthropic" else cfg["ollama_model"]
    return out


def _http_json(url: str, body: dict | None, headers: dict, timeout: int) -> dict:
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, headers={"content-type": "application/json", **headers})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")[:300]
        try:
            detail = json.loads(detail)["error"]["message"]
        except (ValueError, KeyError, TypeError):
            pass
        raise RuntimeError(f"HTTP {e.code}: {detail}") from None
    except urllib.error.URLError as e:
        raise RuntimeError(f"cannot reach {url.split('/api')[0].split('/v1')[0]}: {e.reason}") from None


def chat(system: str, messages: list[dict], cfg: dict | None = None, max_tokens: int = 400) -> str:
    cfg = cfg or load_config()
    if cfg["provider"] == "anthropic":
        if not cfg.get("api_key"):
            raise RuntimeError("no Anthropic API key — set it in the HMI (⚙ LLM) or ANTHROPIC_API_KEY")
        r = _http_json(
            "https://api.anthropic.com/v1/messages",
            {"model": cfg["anthropic_model"], "max_tokens": max_tokens, "system": system, "messages": messages},
            {"x-api-key": cfg["api_key"], "anthropic-version": "2023-06-01"},
            timeout=60,
        )
        return "".join(b.get("text", "") for b in r.get("content", []) if b.get("type") == "text")
    r = _http_json(
        f"{cfg['ollama_host'].rstrip('/')}/api/chat",
        {"model": cfg["ollama_model"], "stream": False, "format": "json", "options": {"temperature": 0},
         "messages": [{"role": "system", "content": system}, *messages]},
        {},
        timeout=180,
    )
    return r["message"]["content"]


def test(cfg: dict | None = None) -> str:
    """Cheap round-trip to check the configuration. Returns a short success message or raises."""
    cfg = cfg or load_config()
    out = chat("Reply with the single word: ok", [{"role": "user", "content": "ping"}], cfg, max_tokens=5)
    model = cfg["anthropic_model"] if cfg["provider"] == "anthropic" else cfg["ollama_model"]
    return f"{cfg['provider']} / {model} answered: {out.strip()[:40]!r}"
