"""The "ask and it does it" agent: a Claude tool-use loop over the MCP tools.

Transport-agnostic: `run` takes an `emit(event)` callback for its steps and a
`confirm(call) -> bool` callback for the tools in `toolhost.needs_confirmation`;
the HTTP server (or a phone client, a CLI) supplies both. The client is any
object with `messages.create(...)` (the anthropic SDK client; tests pass a fake).
"""

from __future__ import annotations

import json
from collections.abc import Callable
from typing import Any

from fb200.mcp_server import INSTRUCTIONS
from fb200_app.toolhost import ToolError, ToolHost, needs_confirmation

MODELS = ("claude-sonnet-5", "claude-opus-5-5")
DEFAULT_MODEL = MODELS[0]
MAX_STEPS = 24
MAX_TOKENS = 16000

SYSTEM = INSTRUCTIONS + """

You are the sound assistant in the FB200 desktop app. The user describes a
sound or a change ("make it brighter", "the low end is muddy", "prepare a slap
preset"); you do it with the tools.
- First call `parameter_docs` (once per conversation) and `get_effects` (and
  `set_eq` / `set_delay` with no arguments to read them) so you know the state.
- Make focused changes, a few values at a time. Say what you change and why in
  one short sentence per step.
- When the pedal is on USB audio, measure: `audio_test` with signal "noise"
  (source "chain") before and after a tonal change, and report the level and
  the octave-band difference. If audio_test fails (no audio device), go on
  without it and say so.
- Never store without the user: `save_preset`, `rename_preset`, `ir_import`,
  `ir_delete`, `long_ir_import`, `long_ir_delete`, `settings` changes and
  console `save` / `factory` / `irdel` ask the user
  for a click first; if the user declines, keep the edit buffer as is.
- There is no firmware flashing here.
Keep the final answer short: what changed (old -> new) and the measured effect."""

Emit = Callable[[dict], None]
Confirm = Callable[[dict], bool]


def _dump(value: Any) -> str:
    return value if isinstance(value, str) else json.dumps(value, indent=1)


class AgentSession:
    """One conversation. Not thread safe: run one `run` at a time."""

    def __init__(self, host: ToolHost, client: Any, model: str = DEFAULT_MODEL,
                 effort: str = "medium", max_steps: int = MAX_STEPS) -> None:
        self.host = host
        self.client = client
        self.model = model
        self.effort = effort
        self.max_steps = max_steps
        self.messages: list[dict] = []
        self.stopped = False

    def reset(self) -> None:
        self.messages = []

    def stop(self) -> None:
        self.stopped = True

    def _request(self) -> Any:
        if self.model not in MODELS:
            raise ValueError(f"model must be one of {', '.join(MODELS)}")
        return self.client.messages.create(
            model=self.model,
            max_tokens=MAX_TOKENS,
            system=[{"type": "text", "text": SYSTEM}],
            tools=self.host.list_tools(),
            messages=self.messages,
            thinking={"type": "adaptive", "display": "summarized"},
            output_config={"effort": self.effort},
            cache_control={"type": "ephemeral"},   # tools + system + history prefix
        )

    def _run_tool(self, block: Any, emit: Emit, confirm: Confirm) -> dict:
        args = dict(block.input or {})
        call = {"id": block.id, "tool": block.name, "input": args}
        reason = needs_confirmation(block.name, args)
        if reason is not None:
            emit({"type": "confirm", **call, "reason": reason})
            approved = confirm({**call, "reason": reason})
            emit({"type": "confirmed", "id": block.id, "approved": approved})
            if not approved:
                emit({"type": "tool_result", "id": block.id, "tool": block.name, "ok": False,
                      "result": "declined by the user"})
                return {"type": "tool_result", "tool_use_id": block.id, "is_error": True,
                        "content": "The user declined this call. Do not retry it; "
                                   "the edit buffer is unchanged by it."}
        try:
            value = self.host.call(block.name, args)
            ok = True
        except ToolError as exc:
            value, ok = str(exc), False
        emit({"type": "tool_result", "id": block.id, "tool": block.name, "ok": ok,
              "result": value})
        return {"type": "tool_result", "tool_use_id": block.id, "content": _dump(value),
                **({} if ok else {"is_error": True})}

    def run(self, text: str, emit: Emit, confirm: Confirm) -> str:
        """Answer one user message; returns the final text."""
        self.stopped = False
        self.messages.append({"role": "user", "content": text})
        final = ""
        for _ in range(self.max_steps):
            if self.stopped:
                emit({"type": "error", "message": "stopped"})
                return final
            response = self._request()
            self.messages.append({"role": "assistant", "content": response.content})
            uses = []
            for block in response.content:
                if block.type == "thinking" and getattr(block, "thinking", ""):
                    emit({"type": "thinking", "text": block.thinking})
                elif block.type == "text" and block.text:
                    final = block.text
                    emit({"type": "text", "text": block.text})
                elif block.type == "tool_use":
                    uses.append(block)
                    emit({"type": "tool_use", "id": block.id, "tool": block.name,
                          "input": dict(block.input or {})})
            usage = getattr(response, "usage", None)
            if usage is not None:
                emit({"type": "usage", "input": getattr(usage, "input_tokens", 0),
                      "output": getattr(usage, "output_tokens", 0),
                      "cache_read": getattr(usage, "cache_read_input_tokens", 0) or 0})
            if response.stop_reason != "tool_use" or not uses:
                if response.stop_reason in ("refusal", "max_tokens"):
                    emit({"type": "error", "message": f"stopped: {response.stop_reason}"})
                emit({"type": "done", "text": final})
                return final
            results = [self._run_tool(b, emit, confirm) for b in uses]
            self.messages.append({"role": "user", "content": results})   # one message
        emit({"type": "error", "message": f"step limit ({self.max_steps}) reached"})
        emit({"type": "done", "text": final})
        return final
