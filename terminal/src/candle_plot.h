#pragma once

namespace tt::ui {

// Draws OHLC candlesticks into the current ImPlot plot (call between
// BeginPlot/EndPlot). xs are epoch seconds; width_sec is the body width.
void PlotCandlestick(const char* label_id, const double* xs, const double* opens,
                     const double* highs, const double* lows, const double* closes,
                     int count, double width_sec);

} // namespace tt::ui
