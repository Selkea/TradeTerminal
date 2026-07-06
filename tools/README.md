# tools/

External runtime tools used alongside TradeTerminal but not part of the build.

## clientportal.gw/  (gitignored)

The IBKR Client Portal Gateway — all market data and orders flow through it.

**Path caveat:** the gateway's embedded web server silently fails to bind
port 5000 when run from a path under OneDrive or containing `+` characters.
Keep the repo on a clean local path (no `+`, not inside OneDrive) and the
gateway works from here in `tools\clientportal.gw`. TradeTerminal
auto-detects two locations for the Sign In > IBKR "Launch gateway" button:
`C:\ibkr\clientportal.gw` first (the VPS/bootstrap default), then
`tools\clientportal.gw`. config.json `ibkr_gateway_cmd` overrides both.

Setup:

1. Download https://download2.interactivebrokers.com/portal/clientportal.gw.zip
2. Extract so that `tools\clientportal.gw\bin\run.bat` exists.
3. Requires Java 8u192+ on PATH (`winget install EclipseAdoptium.Temurin.21.JRE`).

The `scripts/vps_bootstrap.ps1` script performs all three steps automatically
(installing to `C:\ibkr\clientportal.gw`, since a VPS checkout may also sit on
a `+`/OneDrive path).
