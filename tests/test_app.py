"""The desktop app PoC (app/fb200_app) against a fake pedal and a fake Claude
client: no hardware, no network."""

from __future__ import annotations

import json
import shutil
import struct
import subprocess
import time
from pathlib import Path
from types import SimpleNamespace

import pytest
from test_mcp_server import FakeConsole, FakeHid

from fb200 import params
from fb200.mcp_server import Pedal, PedalTools
from fb200.pedal import PRESET_SIZE, FB200Device

pytest.importorskip("mcp")
starlette = pytest.importorskip("starlette")

from fb200_app.agent import DEFAULT_MODEL, MODELS, AgentSession
from fb200_app.chat import ChatRunner
from fb200_app.config import Config
from fb200_app.toolhost import ToolError, ToolHost, needs_confirmation


class AppHid(FakeHid):
    """FakeHid plus what the app uses: 0x96 read preset, 0x99 rename, 0xB0
    settings, 0x63 IR query, 0x67 IR delete."""

    def __init__(self) -> None:
        super().__init__()
        self.names = [f"Preset {i}".encode() for i in range(40)]
        self.settings = bytearray(13)
        self.settings[2] = 13
        self.irs: dict[int, str] = {1: "My Cab"}

    def write_report(self, report: bytes) -> None:
        frames = len(self.frames)
        super().write_report(report)
        for packet in self.frames[frames:]:
            fn, data = packet[0], packet[1:]
            if fn == 0x96:
                preset = bytearray(PRESET_SIZE)
                preset[:len(self.names[data[0]])] = self.names[data[0]]
                self.reply(0x97, bytes([data[0]]) + bytes(preset))
            elif fn == 0x99:
                self.names[data[0]] = bytes(data[1:21]).rstrip(b"\0")
            elif fn == 0xB0:
                self.settings[:] = data[:13]
            elif fn == 0x63:
                slot = struct.unpack_from("<H", data, 1)[0]
                if slot in self.irs:
                    name = self.irs[slot].encode()
                    self.reply(0x64, bytes([1]) + struct.pack("<H", slot) + bytes([1, 0x21, 0])
                               + struct.pack("<H", 50) + name + bytes(50 - len(name)))
                else:
                    self.reply(0x64, bytes([1]) + struct.pack("<H", slot) + bytes([0]))
            elif fn == 0x67:
                slot = struct.unpack_from("<H", data, 1)[0]
                ok = self.irs.pop(slot, None) is not None
                self.reply(0x68, bytes([1]) + struct.pack("<H", slot) + bytes([ok]))

    def reply(self, fn: int, data: bytes) -> None:
        if fn == 0xB0:
            data = bytes(self.settings)
        super().reply(fn, data)


@pytest.fixture
def rig():
    con, hid = FakeConsole(), AppHid()
    tools = PedalTools(Pedal(console_factory=lambda: con,
                             device_factory=lambda: FB200Device(hid)))
    return ToolHost(tools), con, hid


# ---------------------------------------------------------------- parameter docs


def test_parameter_docs_cover_every_field():
    assert params.check_complete() == []
    docs = params.parameter_docs()
    for block in docs["blocks"].values():
        assert block["tool"] in PedalTools.TOOLS
        for spec in block["fields"].values():
            assert spec["min"] <= spec["max"] and spec["meaning"]
    json.dumps(docs)                                     # JSON clean


def test_parameter_docs_is_a_tool_and_a_resource(rig):
    import asyncio

    host = rig[0]
    assert host.call("parameter_docs")["blocks"]["amp"]["fields"]["treble"]["max"] == 100
    contents = asyncio.run(host.server.read_resource("fb200://parameter-docs"))
    text = next(iter(contents)).content
    assert json.loads(text)["eq"]["fields"]["gain_db"]["min"] == -15


# ---------------------------------------------------------------- tool host


def test_tool_schema_is_the_mcp_tools_list(rig):
    host = rig[0]
    schema = host.list_tools()
    assert {t["name"] for t in schema} == set(PedalTools.TOOLS)
    amp = next(t for t in schema if t["name"] == "set_amp")
    assert {"treble", "model"} <= set(amp["input_schema"]["properties"])
    assert [t["name"] for t in schema] == sorted(t["name"] for t in schema)   # cache-stable


def test_confirmation_policy():
    assert needs_confirmation("save_preset", {})
    assert needs_confirmation("ir_import", {"slot": 1, "wav_path": "x"})
    assert needs_confirmation("ir_delete", {"slot": 1})
    assert needs_confirmation("long_ir_import", {"slot": 20, "wav_path": "x"})
    assert needs_confirmation("long_ir_delete", {"slot": 20})
    assert needs_confirmation("console", {"command": "irdel 20"})
    assert not needs_confirmation("long_ir_list", {})
    assert needs_confirmation("rename_preset", {"index": 1, "name": "x"})
    assert needs_confirmation("console", {"command": "factory yes"})
    assert needs_confirmation("console", {"command": "save"})
    assert needs_confirmation("settings", {"bt_audio": 1})
    assert needs_confirmation("crash_dump", {"clear": True})
    assert needs_confirmation("settings", {}) is None
    assert needs_confirmation("console", {"command": "stats"}) is None
    assert needs_confirmation("set_amp", {"treble": 70}) is None


def test_hid_app_tools(rig):
    host, _, hid = rig
    listed = host.call("preset_list")
    assert listed["current"] == 3 and listed["presets"][5] == {"index": 5, "name": "Preset 5"}
    assert host.call("rename_preset", {"index": 5, "name": "Slap!"})["name"] == "Slap!"
    assert hid.names[5] == b"Slap!"
    assert host.call("settings")["input_gain"] == 13
    assert host.call("settings", {"input_gain": 14, "ring_level": 50}) == {
        "cab_global": 0, "input_gain": 14, "tuner": 0, "bt_audio": 0, "ring_color": 0,
        "ring_level": 50}
    with pytest.raises(ToolError, match="input_gain must be 0..25"):
        host.call("settings", {"input_gain": 40})
    assert host.call("ir_list")[0] == {"slot": 1, "name": "My Cab"}
    assert host.call("ir_delete", {"slot": 1}) == {"slot": 1, "deleted": True}
    assert host.call("ir_list")[0]["name"] is None


def test_tool_errors_and_flash_refused(rig):
    host = rig[0]
    with pytest.raises(ToolError, match="fb200 update"):
        host.call("console", {"command": "fwbegin 1 2"})
    with pytest.raises(ToolError, match="unknown tool"):
        host.call("flash_firmware", {})


# ---------------------------------------------------------------- agent loop


def block(kind: str, **kw):
    return SimpleNamespace(type=kind, **kw)


def response(*content, stop="tool_use"):
    return SimpleNamespace(content=list(content), stop_reason=stop,
                           usage=SimpleNamespace(input_tokens=10, output_tokens=5,
                                                 cache_read_input_tokens=0))


class FakeClaude:
    """messages.create returns the scripted responses; records every request."""

    def __init__(self, script) -> None:
        self.script = list(script)
        self.requests: list[dict] = []
        self.messages = self

    def create(self, **kwargs):
        self.requests.append({**kwargs, "messages": list(kwargs["messages"])})
        return self.script.pop(0)


def slap_script():
    return [
        response(block("thinking", thinking="read the state first", signature="s"),
                 block("tool_use", id="t1", name="parameter_docs", input={}),
                 block("tool_use", id="t2", name="get_effects", input={})),
        response(block("text", text="Raising the treble and compressing."),
                 block("tool_use", id="t3", name="set_amp", input={"treble": 70}),
                 block("tool_use", id="t4", name="set_comp",
                       input={"enabled": True, "threshold": 35, "ratio": 60})),
        response(block("tool_use", id="t5", name="save_preset", input={})),
        response(block("text", text="Done: treble 50 -> 70, comp on."), stop="end_turn"),
    ]


def run_agent(rig, approve: bool):
    host = rig[0]
    client = FakeClaude(slap_script())
    events, asked = [], []
    session = AgentSession(host, client)
    final = session.run("prepare a slap preset", events.append,
                        lambda call: asked.append(call) or approve)
    return session, client, events, asked, final


def test_agent_loop_calls_tools_and_declines_save(rig):
    _, con, hid = rig
    _, client, events, asked, final = run_agent(rig, approve=False)
    assert final == "Done: treble 50 -> 70, comp on."
    req = client.requests[0]
    assert req["model"] == DEFAULT_MODEL and req["tools"] == rig[0].list_tools()
    assert req["thinking"]["type"] == "adaptive" and req["cache_control"] == {"type": "ephemeral"}
    assert req["system"][0]["text"].startswith("Tools for the FLAMMA FB200")
    kinds = [e["type"] for e in events]
    assert kinds[:3] == ["thinking", "tool_use", "tool_use"]
    assert "confirm" in kinds and kinds[-1] == "done"
    assert [a["tool"] for a in asked] == ["save_preset"]
    assert "save" not in con.sent                               # declined: not stored
    amp = struct.unpack_from("<8H", hid.edit, 0x2C)
    assert amp[6] == 70                                         # treble written over HID
    comp = struct.unpack_from("<6H", hid.edit, 0x14)
    assert comp[0] == 1 and comp[3] == 35 and comp[4] == 60
    # all tool results of one turn go back in ONE user message, errors flagged
    second = client.requests[1]["messages"]
    assert second[-1]["role"] == "user"
    assert [r["tool_use_id"] for r in second[-1]["content"]] == ["t1", "t2"]
    declined = client.requests[3]["messages"][-1]["content"][0]
    assert declined["is_error"] and "declined" in declined["content"]
    results = {e["id"]: e for e in events if e["type"] == "tool_result"}
    assert results["t3"]["ok"] and results["t3"]["result"]["treble"] == 70


def test_agent_loop_save_after_approval(rig):
    con = rig[1]
    *_, asked, _ = run_agent(rig, approve=True)
    assert asked and con.sent[-1] == "save"


def test_agent_tool_error_goes_back_to_the_model(rig):
    host = rig[0]
    client = FakeClaude([
        response(block("tool_use", id="e1", name="preset", input={"index": 99})),
        response(block("text", text="That preset does not exist."), stop="end_turn"),
    ])
    events = []
    AgentSession(host, client).run("go to preset 99", events.append, lambda c: True)
    result = client.requests[1]["messages"][-1]["content"][0]
    assert result["is_error"] and "0..39" in result["content"]
    assert any(e["type"] == "tool_result" and not e["ok"] for e in events)


def test_agent_rejects_unknown_model(rig):
    session = AgentSession(rig[0], FakeClaude([]), model="gpt-4")
    with pytest.raises(ValueError, match="model must be one of"):
        session.run("hi", lambda e: None, lambda c: True)
    assert "claude-opus-5-5" in MODELS


def test_agent_step_limit(rig):
    loop = [response(block("tool_use", id=f"l{i}", name="get_effects", input={}))
            for i in range(3)]
    events = []
    AgentSession(rig[0], FakeClaude(loop), max_steps=3).run("x", events.append, lambda c: 1)
    assert events[-2]["type"] == "error" and "step limit" in events[-2]["message"]


# ---------------------------------------------------------------- HTTP


def wait_done(client, after=0, timeout=5.0):
    deadline, events = time.monotonic() + timeout, []
    while time.monotonic() < deadline:
        events += client.get(f"/api/chat/events?after={after + len(events)}&wait=1").json()[
            "events"]
        if any(e["type"] == "done" for e in events):
            return events
    raise AssertionError(f"no done event: {events}")


@pytest.fixture
def http(rig, tmp_path, monkeypatch):
    from fb200_app.server import create_app
    from starlette.testclient import TestClient

    monkeypatch.delenv("ANTHROPIC_API_KEY", raising=False)
    fake = FakeClaude(slap_script())
    app = create_app(rig[0], Config(tmp_path / "app.json"), client_factory=lambda: fake,
                     upload_dir=tmp_path)
    with TestClient(app) as client:
        client.headers["X-FB200-App"] = "1"
        yield client, rig, fake


def test_http_status_docs_and_tools(http):
    client, (_, con, _), _ = http
    status = client.get("/api/status").json()
    assert status["connected"] and status["console"] and status["preset"]["index"] == 3
    docs = client.get("/api/docs").json()
    assert "set_eq" in docs["tools"] and "blocks" in docs["parameters"]
    ok = client.post("/api/tools/set_amp", json={"args": {"treble": 60}}).json()
    assert ok["ok"] and ok["result"]["treble"] == 60
    bad = client.post("/api/tools/preset", json={"args": {"index": 50}})
    assert bad.status_code == 400 and "0..39" in bad.json()["error"]
    need = client.post("/api/tools/save_preset", json={"args": {}})
    assert need.status_code == 409 and "flash" in need.json()["confirm"]
    assert "save" not in con.sent
    done = client.post("/api/tools/save_preset", json={"args": {}, "confirmed": True})
    assert done.json()["ok"] and con.sent[-1] == "save"


def test_http_needs_the_app_header(http):
    client = http[0]
    r = client.post("/api/tools/set_amp", json={"args": {"treble": 1}},
                    headers={"X-FB200-App": ""})
    assert r.status_code == 403


def test_http_upload_checks_wav(http, tmp_path):
    client = http[0]
    assert client.post("/api/upload?filename=x.wav", content=b"not a wav").status_code == 400
    wav = b"RIFF\x24\x00\x00\x00WAVEfmt " + bytes(28)
    r = client.post("/api/upload?filename=../../evil.wav", content=wav).json()
    assert r["ok"] and Path(r["path"]).parent == tmp_path and r["name"] == "evil"


def test_http_config_never_returns_the_key(http):
    client = http[0]
    assert client.get("/api/config").json()["key_set"] is False
    r = client.post("/api/config", json={"model": "claude-opus-5-5", "api_key": "sk-test"})
    assert r.json()["key_set"] and "sk-test" not in r.text
    cfg = Config(Path(r.json()["path"]))
    assert cfg.model == "claude-opus-5-5" and cfg.api_key == "sk-test"
    assert Path(r.json()["path"]).stat().st_mode & 0o077 == 0
    assert client.post("/api/config", json={"model": "nope"}).status_code == 400


def test_http_chat_with_confirmation(http):
    client, (_, con, _), fake = http
    assert client.post("/api/chat", json={"text": "prepare a slap preset"}).json()["ok"]
    events = []
    deadline = time.monotonic() + 5
    while not any(e["type"] == "confirm" for e in events):
        assert time.monotonic() < deadline
        events += client.get(f"/api/chat/events?after={len(events)}&wait=1").json()["events"]
    ask = next(e for e in events if e["type"] == "confirm")
    assert ask["tool"] == "save_preset"
    assert client.post("/api/chat/confirm", json={"id": ask["id"], "approve": True}).json()["ok"]
    events += wait_done(client, after=len(events))
    assert con.sent[-1] == "save"
    assert events[0]["type"] == "user" and events[-1]["type"] == "done"
    assert fake.requests[0]["model"] == DEFAULT_MODEL


def test_chat_runner_stop_declines_pending_confirmation(rig):
    fake = FakeClaude(slap_script())
    runner = ChatRunner(rig[0], lambda: fake, confirm_timeout=5)
    runner.start("slap")
    deadline = time.monotonic() + 5
    while not runner.pending:
        assert time.monotonic() < deadline
        time.sleep(0.01)
    runner.stop()
    runner.join(5)
    assert not runner.busy and "save" not in rig[1].sent


def test_ui_smoke(http):
    client = http[0]
    page = client.get("/")
    assert page.status_code == 200 and "/static/app.js" in page.text
    js = client.get("/static/app.js")
    assert js.status_code == 200 and "api/chat/events" in js.text
    assert client.get("/static/style.css").status_code == 200
    node = shutil.which("node")
    if node:                                    # the UI script parses
        src = Path(__file__).parents[1] / "app" / "fb200_app" / "static" / "app.js"
        subprocess.run([node, "--check", str(src)], check=True, capture_output=True, timeout=20)


def test_rejects_foreign_host_header(http):
    """DNS rebinding: a request that reaches 127.0.0.1 under another name is refused."""
    client = http[0]
    assert client.get("/api/status", headers={"Host": "evil.example"}).status_code == 400
    assert client.get("/api/status").status_code == 200


def test_http_status_says_the_console_port_is_busy(tmp_path, monkeypatch):
    pty = pytest.importorskip("pty")
    import os

    from fb200_app.server import create_app
    from starlette.testclient import TestClient

    from fb200.console import Console

    master, slave = pty.openpty()
    port = os.ttyname(slave)
    holder = Console(port)
    try:
        hid = AppHid()
        host = ToolHost(PedalTools(Pedal(port, device_factory=lambda: FB200Device(hid))))
        monkeypatch.delenv("ANTHROPIC_API_KEY", raising=False)
        app = create_app(host, Config(tmp_path / "app.json"), client_factory=lambda: None,
                         upload_dir=tmp_path)
        with TestClient(app) as client:
            status = client.get("/api/status").json()
        assert status["connected"] and status["console"] is False
        assert f"{port} is busy" in status["error"] and "Close it" in status["error"]
    finally:
        holder.close()
        os.close(master)
        os.close(slave)
