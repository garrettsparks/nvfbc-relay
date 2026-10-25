#include "FlowProbeLoops.h"
#include "OutputWindow.h"
#include <SimpleLogger.h>

#include <algorithm>
#include <cmath>

namespace flowprobe {

double Percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t i = (size_t)std::min((double)(v.size() - 1), q * (double)(v.size() - 1) + 0.5);
    return v[i];
}

bool CheckFlow(const char* label, const char* direction, const unsigned char* data,
               size_t rowPitch, int cellsX, int cellsY, int grid, float wantX, float wantY) {
    const int margin = (16 + grid - 1) / grid;
    double sx = 0.0, sy = 0.0;
    long long n = 0, ok = 0;
    for (int y = margin; y + margin < cellsY; y++) {
        const short* row = (const short*)(data + (size_t)y * rowPitch);
        for (int x = margin; x + margin < cellsX; x++) {
            const float vx = row[2 * x] / 32.0f;
            const float vy = row[2 * x + 1] / 32.0f;
            sx += vx;
            sy += vy;
            n++;
            if (std::fabs(vx - wantX) <= 0.5f && std::fabs(vy - wantY) <= 0.5f) ok++;
        }
    }
    const double right = n ? (double)ok / (double)n : 0.0;
    LOG("ofprobe: %s: %s mean (%.2f, %.2f) %.1f%% right (want (%.0f, %.0f))", label, direction,
        n ? sx / n : 0.0, n ? sy / n : 0.0, 100.0 * right, wantX, wantY);
    return right >= 0.9;
}

void RunPaced(const char* label, int seconds, int rate, const FlowStep& step) {
    LARGE_INTEGER freq, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    const LONGLONG period = freq.QuadPart / rate;
    long long n = 0;
    std::vector<double> returned, done;
    for (int sec = 0; sec < seconds; sec++) {
        returned.clear();
        done.clear();
        for (int k = 0; k < rate; k++, n++) {
            const LONGLONG due = start.QuadPart + n * period;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            while (now.QuadPart < due) {
                const LONGLONG ms = (due - now.QuadPart) * 1000 / freq.QuadPart;
                if (ms > 1) Sleep((DWORD)(ms - 1));
                else YieldProcessor();
                QueryPerformanceCounter(&now);
            }
            double r = 0.0, d = 0.0;
            if (!step(&r, &d)) {
                LOG("ofprobe: %s loop: a flow failed or did not finish; stopping", label);
                return;
            }
            returned.push_back(r);
            done.push_back(d);
        }
        LOG("ofprobe: %s loop second %d: %d flows, returned p50 %.2f max %.2f ms, done p50 %.2f "
            "max %.2f ms", label, sec + 1, rate, Percentile(returned, 0.5),
            Percentile(returned, 1.0), Percentile(done, 0.5), Percentile(done, 1.0));
        if (!PumpMessages()) {
            LOG("ofprobe: the output window closed; stopping");
            return;
        }
    }
}

}  // namespace flowprobe
