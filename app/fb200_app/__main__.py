"""`fb200-app` / `python -m fb200_app`: serve the app and open it in the browser."""

from __future__ import annotations

import argparse
import threading
import webbrowser


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="fb200-app", description=__doc__)
    parser.add_argument("--host", default="127.0.0.1",
                        help="bind address (default 127.0.0.1; the API has no login)")
    parser.add_argument("--port", type=int, default=8200)
    parser.add_argument("--console", help="CDC console device (default: auto)")
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args(argv)

    import uvicorn
    from fb200.mcp_server import Pedal, PedalTools

    from fb200_app.server import create_app
    from fb200_app.toolhost import ToolHost

    app = create_app(ToolHost(PedalTools(Pedal(args.console))))
    url = f"http://{args.host}:{args.port}/"
    if not args.no_browser:
        threading.Timer(1.0, webbrowser.open, args=(url,)).start()
    print(f"fb200-app on {url}")
    uvicorn.run(app, host=args.host, port=args.port, log_level="warning")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
