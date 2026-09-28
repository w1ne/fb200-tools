"""HTTP API + static UI (Starlette). Binds to 127.0.0.1 by default.

Every POST needs the header `X-FB200-App: 1`: a custom header forces a CORS
preflight, which this server never answers, so other web pages cannot drive
the pedal through it.
"""

from __future__ import annotations

import contextlib
import tempfile
from pathlib import Path
from typing import Any

from starlette.applications import Starlette
from starlette.middleware import Middleware
from starlette.middleware.trustedhost import TrustedHostMiddleware
from starlette.requests import Request
from starlette.responses import FileResponse, JSONResponse
from starlette.routing import Mount, Route
from starlette.staticfiles import StaticFiles

from fb200_app.chat import Busy, ChatRunner
from fb200_app.config import Config
from fb200_app.toolhost import ToolError, ToolHost, needs_confirmation

STATIC = Path(__file__).parent / "static"
APP_HEADER = "x-fb200-app"
MAX_UPLOAD = 16 * 1024 * 1024


def anthropic_factory(config: Config):
    def make():
        key = config.api_key
        if not key:
            raise ValueError("no Anthropic API key: set ANTHROPIC_API_KEY or enter one in "
                             "Settings")
        import anthropic

        return anthropic.Anthropic(api_key=key)
    return make


def create_app(host: ToolHost | None = None, config: Config | None = None,
               client_factory=None, upload_dir: Path | None = None) -> Starlette:
    host = host or ToolHost()
    config = config or Config()
    runner = ChatRunner(host, client_factory or anthropic_factory(config))
    uploads = upload_dir or Path(tempfile.mkdtemp(prefix="fb200-app-"))

    def err(status: int, message: str, **extra: Any) -> JSONResponse:
        return JSONResponse({"ok": False, "error": message, **extra}, status_code=status)

    async def body(request: Request) -> dict:
        if request.headers.get(APP_HEADER) != "1":
            raise PermissionError("missing X-FB200-App header")
        try:
            data = await request.json()
        except ValueError:
            data = {}
        return data if isinstance(data, dict) else {}

    def guarded(fn):
        async def endpoint(request: Request):
            try:
                return await fn(request)
            except PermissionError as exc:
                return err(403, str(exc))
            except Busy as exc:
                return err(409, str(exc))
            except ValueError as exc:
                return err(400, str(exc))
        return endpoint

    async def index(_request: Request):
        return FileResponse(STATIC / "index.html")

    def status(_request: Request):
        out: dict[str, Any] = {"connected": False}
        try:
            out["info"] = host.call("pedal_info")
            out["connected"] = True
        except ToolError as exc:
            out["error"] = str(exc)
        try:
            out["preset"] = host.call("preset")
            out["console"] = True
        except ToolError as exc:
            out["console"] = False
            out.setdefault("error", str(exc))
        return JSONResponse(out)

    def docs(_request: Request):
        return JSONResponse({"parameters": host.call("parameter_docs"),
                             "tools": [t["name"] for t in host.list_tools()]})

    async def tool(request: Request):
        name = request.path_params["name"]
        data = await body(request)
        args = data.get("args") or {}
        reason = needs_confirmation(name, args)
        if reason is not None and not data.get("confirmed"):
            return err(409, f"needs confirmation: {reason}", confirm=reason)
        try:
            return JSONResponse({"ok": True, "result": await _threaded(host.call, name, args)})
        except ToolError as exc:
            return err(400, str(exc))

    async def upload(request: Request):
        if request.headers.get(APP_HEADER) != "1":
            raise PermissionError("missing X-FB200-App header")
        raw = await request.body()
        if not raw or len(raw) > MAX_UPLOAD:
            raise ValueError("empty or too large upload")
        if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
            raise ValueError("not a WAV file")
        name = Path(request.query_params.get("filename", "ir.wav")).name or "ir.wav"
        path = uploads / name
        path.write_bytes(raw)
        return JSONResponse({"ok": True, "path": str(path), "name": Path(name).stem})

    async def get_config(_request: Request):
        return JSONResponse(config.public())

    async def set_config(request: Request):
        data = await body(request)
        config.update(model=data.get("model"), api_key=data.get("api_key"))
        if not runner.busy:
            runner.session = None             # a new key or model: a new client
        return JSONResponse(config.public())

    async def chat(request: Request):
        data = await body(request)
        text = str(data.get("text", "")).strip()
        if not text:
            raise ValueError("empty message")
        runner.start(text, data.get("model") or config.model)
        return JSONResponse({"ok": True})

    def chat_events(request: Request):
        after = int(request.query_params.get("after", 0))
        wait = min(float(request.query_params.get("wait", 0)), 25.0)
        return JSONResponse({"events": runner.events_after(after, wait), "busy": runner.busy})

    async def chat_confirm(request: Request):
        data = await body(request)
        ok = runner.answer(str(data.get("id")), bool(data.get("approve")))
        return JSONResponse({"ok": ok}, status_code=200 if ok else 404)

    async def chat_stop(request: Request):
        await body(request)
        runner.stop()
        return JSONResponse({"ok": True})

    async def chat_reset(request: Request):
        await body(request)
        runner.reset()
        return JSONResponse({"ok": True})

    routes = [
        Route("/", index),
        Route("/api/status", status),
        Route("/api/docs", docs),
        Route("/api/tools/{name}", guarded(tool), methods=["POST"]),
        Route("/api/upload", guarded(upload), methods=["POST"]),
        Route("/api/config", get_config),
        Route("/api/config", guarded(set_config), methods=["POST"]),
        Route("/api/chat", guarded(chat), methods=["POST"]),
        Route("/api/chat/events", chat_events),
        Route("/api/chat/confirm", guarded(chat_confirm), methods=["POST"]),
        Route("/api/chat/stop", guarded(chat_stop), methods=["POST"]),
        Route("/api/chat/reset", guarded(chat_reset), methods=["POST"]),
        Mount("/static", StaticFiles(directory=STATIC), name="static"),
    ]
    @contextlib.asynccontextmanager
    async def lifespan(_app):
        yield
        runner.stop()
        host.close()

    # Host check: a DNS-rebinding page (evil.example -> 127.0.0.1) is same-origin
    # and passes the header guard, but its Host header is not ours
    local = TrustedHostMiddleware, ["127.0.0.1", "localhost", "testserver"]
    app = Starlette(routes=routes, lifespan=lifespan,
                    middleware=[Middleware(local[0], allowed_hosts=local[1])])
    app.state.host, app.state.runner, app.state.config = host, runner, config
    return app


async def _threaded(fn, *args):
    from starlette.concurrency import run_in_threadpool

    return await run_in_threadpool(fn, *args)
