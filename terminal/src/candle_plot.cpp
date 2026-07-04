#include "candle_plot.h"

#include "implot.h"
#include "implot_internal.h"  // BeginItem/EndItem/FitPoint (ImPlot demo pattern)

namespace tt::ui {

void PlotCandlestick(const char* label_id, const double* xs, const double* opens,
                     const double* highs, const double* lows, const double* closes,
                     int count, double width_sec) {
    const ImVec4 bull(0.20f, 0.78f, 0.45f, 1.0f);
    const ImVec4 bear(0.86f, 0.30f, 0.30f, 1.0f);

    if (!ImPlot::BeginItem(label_id)) return;

    if (ImPlot::FitThisFrame()) {
        for (int i = 0; i < count; ++i) {
            ImPlot::FitPoint(ImPlotPoint(xs[i], lows[i]));
            ImPlot::FitPoint(ImPlotPoint(xs[i], highs[i]));
        }
    }

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const double half = width_sec * 0.5;
    for (int i = 0; i < count; ++i) {
        const bool up = closes[i] >= opens[i];
        const ImU32 col = ImGui::GetColorU32(up ? bull : bear);

        const ImVec2 lo = ImPlot::PlotToPixels(xs[i], lows[i]);
        const ImVec2 hi = ImPlot::PlotToPixels(xs[i], highs[i]);
        dl->AddLine(lo, hi, col, 1.0f);

        ImVec2 a = ImPlot::PlotToPixels(xs[i] - half, opens[i]);
        ImVec2 b = ImPlot::PlotToPixels(xs[i] + half, closes[i]);
        if (a.y == b.y) b.y += 1.0f;  // doji: keep the body visible
        dl->AddRectFilled(a, b, col);
    }

    ImPlot::EndItem();
}

} // namespace tt::ui
