import argparse
import asyncio
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description="TradeTerminal data sidecar")
    ap.add_argument("--port", type=int, default=0,
                    help="listen port (0 = OS-assigned, printed as TT_PORT=<n>)")
    ap.add_argument("--db", default=None, help="override cache db path")
    args = ap.parse_args()

    from server import serve
    try:
        asyncio.run(serve(args.port, args.db))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
