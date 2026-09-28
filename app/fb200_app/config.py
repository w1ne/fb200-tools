"""App settings: the model and the Anthropic API key.

The key comes from ANTHROPIC_API_KEY, else from the local config file
(~/.config/fb200/app.json, mode 0600, outside the repo). It is never sent to
the browser: the UI only sees whether a key is set.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

from fb200_app.agent import DEFAULT_MODEL, MODELS


def default_path() -> Path:
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "fb200" / "app.json"


class Config:
    def __init__(self, path: Path | None = None) -> None:
        self.path = path or default_path()
        self.data: dict = {}
        try:
            self.data = json.loads(self.path.read_text())
        except (OSError, ValueError):
            self.data = {}

    @property
    def model(self) -> str:
        m = self.data.get("model", DEFAULT_MODEL)
        return m if m in MODELS else DEFAULT_MODEL

    @property
    def api_key(self) -> str | None:
        return os.environ.get("ANTHROPIC_API_KEY") or self.data.get("api_key") or None

    def key_source(self) -> str | None:
        if os.environ.get("ANTHROPIC_API_KEY"):
            return "env"
        return "file" if self.data.get("api_key") else None

    def update(self, model: str | None = None, api_key: str | None = None) -> None:
        if model is not None:
            if model not in MODELS:
                raise ValueError(f"model must be one of {', '.join(MODELS)}")
            self.data["model"] = model
        if api_key is not None:
            if api_key:
                self.data["api_key"] = api_key.strip()
            else:
                self.data.pop("api_key", None)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            json.dump(self.data, f)
        os.replace(tmp, self.path)

    def public(self) -> dict:
        return {"model": self.model, "models": list(MODELS), "key_set": self.api_key is not None,
                "key_source": self.key_source(), "path": str(self.path)}
