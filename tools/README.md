# tools/

External runtime tools that live alongside TradeTerminal but are not part
of the build.

## clientportal.gw/  (gitignored)

The IBKR Client Portal Gateway. Setup:

1. Download https://download2.interactivebrokers.com/portal/clientportal.gw.zip
2. Extract it here so that `tools/clientportal.gw/bin/run.bat` exists.
3. Requires Java 8u192+ on PATH (`winget install EclipseAdoptium.Temurin.21.JRE`).

TradeTerminal auto-detects this location: the Account > Sign In > IBKR
dialog gets a "Launch gateway" button with no configuration needed
(config.json `ibkr_gateway_cmd` still overrides if set).
