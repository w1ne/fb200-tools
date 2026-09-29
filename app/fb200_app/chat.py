"""Runs the agent in a background thread for a polling client.

Events get a sequence number; a client reads them with `events_after(n)`
(long poll). A confirmation blocks the agent thread until `answer(id, ...)`
or the timeout (= declined). No HTTP here: any transport can drive it.
"""

from __future__ import annotations

import threading
from collections.abc import Callable
from typing import Any

from fb200_app.agent import MODELS, AgentSession
from fb200_app.toolhost import ToolHost

CONFIRM_TIMEOUT_S = 300.0


class Busy(Exception):
    pass


class ChatRunner:
    def __init__(self, host: ToolHost, client_factory: Callable[[], Any],
                 confirm_timeout: float = CONFIRM_TIMEOUT_S) -> None:
        self.host = host
        self.client_factory = client_factory
        self.confirm_timeout = confirm_timeout
        self.cond = threading.Condition()
        self.events: list[dict] = []
        self.pending: dict[str, dict] = {}
        self.session: AgentSession | None = None
        self.thread: threading.Thread | None = None

    @property
    def busy(self) -> bool:
        return self.thread is not None and self.thread.is_alive()

    def emit(self, event: dict) -> None:
        with self.cond:
            self.events.append({"seq": len(self.events), **event})
            self.cond.notify_all()

    def events_after(self, after: int, wait: float = 0.0) -> list[dict]:
        with self.cond:
            if len(self.events) <= after and wait > 0:
                self.cond.wait_for(lambda: len(self.events) > after, timeout=wait)
            return self.events[after:]

    def confirm(self, call: dict) -> bool:
        slot = {"event": threading.Event(), "approved": False}
        with self.cond:
            self.pending[call["id"]] = slot
        answered = slot["event"].wait(self.confirm_timeout)
        with self.cond:
            self.pending.pop(call["id"], None)
        return answered and slot["approved"]

    def answer(self, call_id: str, approve: bool) -> bool:
        with self.cond:
            slot = self.pending.get(call_id)
        if slot is None:
            return False
        slot["approved"] = bool(approve)
        slot["event"].set()
        return True

    def start(self, text: str, model: str | None = None) -> None:
        if self.busy:
            raise Busy("the assistant is still working")
        if model is not None and model not in MODELS:
            raise ValueError(f"model must be one of {', '.join(MODELS)}")
        if self.session is None:
            self.session = AgentSession(self.host, self.client_factory())
        if model is not None:
            self.session.model = model
        self.emit({"type": "user", "text": text, "model": self.session.model})
        self.thread = threading.Thread(target=self._run, args=(text,), daemon=True)
        self.thread.start()

    def _run(self, text: str) -> None:
        try:
            self.session.run(text, self.emit, self.confirm)
        except Exception as exc:  # noqa: BLE001 - API/network errors end the run, shown in chat
            self.emit({"type": "error", "message": f"{type(exc).__name__}: {exc}"})
            self.emit({"type": "done", "text": ""})

    def stop(self) -> None:
        if self.session is not None:
            self.session.stop()
        with self.cond:
            slots = list(self.pending.values())
        for slot in slots:              # a waiting confirmation counts as declined
            slot["event"].set()

    def reset(self) -> None:
        if self.busy:
            raise Busy("stop the assistant first")
        self.session = None
        self.emit({"type": "reset"})

    def join(self, timeout: float | None = None) -> None:
        if self.thread is not None:
            self.thread.join(timeout)
