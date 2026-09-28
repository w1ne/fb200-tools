"""The fb200 MCP server in process: its tools/list is the only tool schema.

The UI editors and the agent both go through `ToolHost.call`, so they share
the one pedal lock, the argument validation of the MCP SDK and the tool error
handling of `fb200.mcp_server.build_server`. `needs_confirmation` is the one
policy for tools that store to flash or can lose data.
"""

from __future__ import annotations

import asyncio
import json
from typing import Any

from fb200.mcp_server import FLASH_COMMANDS, Pedal, PedalTools, build_server

# Tools that write flash or overwrite user data: the user confirms each call.
CONFIRM_TOOLS = {
    "save_preset": "stores the edit buffer into the current preset (flash)",
    "rename_preset": "renames and stores a preset (flash)",
    "ir_import": "overwrites a user IR slot (flash)",
    "ir_delete": "deletes a user IR slot (flash)",
    "settings": "changes the global settings",
}
# Console commands that store, reset, reboot or poke memory.
CONFIRM_CONSOLE = {
    "save": "stores the preset (flash)",
    "factory": "factory reset: erases ALL presets and IRs",
    "crash": "crashes the pedal (fault test)", "hang": "hangs the pedal (fault test)",
    "reset": "resets the pedal", "reboot": "reboots the pedal",
    "recovery": "reboots into recovery", "poke": "writes memory", "poke32": "writes memory",
    "crashclear": "erases the crash dump", "fwtest": "firmware self test",
    "bt": "Bluetooth module commands",
}


def needs_confirmation(name: str, args: dict | None) -> str | None:
    """Why this call needs the user's click, or None for a free call."""
    args = args or {}
    if name == "settings" and not any(v is not None for v in args.values()):
        return None                                  # read only
    if name in CONFIRM_TOOLS:
        return CONFIRM_TOOLS[name]
    if name == "crash_dump" and args.get("clear"):
        return "erases the crash dump"
    if name == "console":
        words = str(args.get("command", "")).split()
        if words and words[0] in FLASH_COMMANDS:
            return "flash streaming (refused by the server)"
        if words and words[0] in CONFIRM_CONSOLE:
            return CONFIRM_CONSOLE[words[0]]
    return None


class ToolError(Exception):
    pass


def _result_value(result: Any) -> Any:
    """CallToolResult (mcp 2.x) or a content sequence (1.x) -> JSON value."""
    content = getattr(result, "content", None)
    if content is None and isinstance(result, tuple):
        content = result[0]
    if content is None:
        content = result
    texts = [c.text for c in content if getattr(c, "type", None) == "text"]
    if getattr(result, "is_error", False) or getattr(result, "isError", False):
        raise ToolError("\n".join(texts))
    if len(texts) == 1:
        try:
            return json.loads(texts[0])
        except ValueError:
            return texts[0]
    values = []
    for t in texts:
        try:
            values.append(json.loads(t))
        except ValueError:
            values.append(t)
    return values


class ToolHost:
    def __init__(self, tools: PedalTools | None = None) -> None:
        self.tools = tools or PedalTools(Pedal())
        self.server = build_server(self.tools)
        self._schema: list[dict] | None = None

    def list_tools(self) -> list[dict]:
        """tools/list as Messages API tool definitions (name, description, input_schema)."""
        if self._schema is None:
            listed = asyncio.run(self.server.list_tools())
            schema = []
            for t in listed:
                input_schema = getattr(t, "inputSchema", None) or getattr(t, "input_schema")
                schema.append({"name": t.name, "description": t.description or "",
                               "input_schema": input_schema})
            self._schema = sorted(schema, key=lambda d: d["name"])   # stable: cacheable
        return self._schema

    def call(self, name: str, args: dict | None = None) -> Any:
        """Run one tool; raise ToolError with the server's message on failure."""
        if name not in {t["name"] for t in self.list_tools()}:
            raise ToolError(f"unknown tool {name!r}")
        try:
            result = asyncio.run(self.server.call_tool(name, dict(args or {})))
        except ToolError:
            raise
        except Exception as exc:          # the SDK's ToolError and validation errors
            raise ToolError(str(exc)) from exc
        return _result_value(result)

    def close(self) -> None:
        self.tools.pedal.close()
