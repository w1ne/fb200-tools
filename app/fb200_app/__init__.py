"""FB200 desktop app (PoC): a local web UI and a Claude agent over the MCP tools.

Layers (each one knows only the one below it):
  server.py   HTTP + the static UI (any browser client: desktop now, a phone later)
  agent.py    the Claude tool-use loop; emits events, asks for confirmations
  toolhost.py the in-process MCP server (fb200.mcp_server): tools/list is the
              only tool schema, call_tool the only way to touch the pedal
"""
