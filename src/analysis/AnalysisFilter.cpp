#include "AnalysisFilter.h"

#include "Lang.h"
#include "media/VideoFrame.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace {

double smoothstep(double e0, double e1, double x)
{
    const double t = std::clamp((x - e0) / std::max(1e-6, e1 - e0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

struct Rgb {
    double r, g, b;
};

inline Rgb rgbAt(const QImage& img, int x, int y)
{
    x = std::clamp(x, 0, img.width() - 1);
    y = std::clamp(y, 0, img.height() - 1);
    const QRgb p = reinterpret_cast<const QRgb*>(img.constScanLine(y))[x];
    return { qRed(p) / 255.0, qGreen(p) / 255.0, qBlue(p) / 255.0 };
}

inline double lumaOf(const Rgb& c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }

inline Rgb mix(const Rgb& a, const Rgb& b, double t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }

inline Rgb rgbOf(const QColor& c) { return { c.redF(), c.greenF(), c.blueF() }; }

inline QRgb pack(const Rgb& c)
{
    auto u = [](double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    return qRgb(u(c.r), u(c.g), u(c.b));
}

// Largest channel difference, averaged over a pixel and its four neighbours (cleans up noise).
double difference(const QImage& a, const QImage& b, int x, int y)
{
    static const int dx[] = { 0, 1, -1, 0, 0 }, dy[] = { 0, 0, 0, 1, -1 };
    double sum = 0;
    for (int i = 0; i < 5; ++i) {
        const Rgb p = rgbAt(a, x + dx[i], y + dy[i]), q = rgbAt(b, x + dx[i], y + dy[i]);
        sum += std::max({ std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b) });
    }
    return sum / 5.0;
}

} // namespace

QJsonObject AnalysisFilter::toJson() const
{
    QJsonObject o;
    o["mode"] = int(mode);
    QJsonArray t;
    for (double v : thresholds) t.append(v);
    o["thresholds"] = t;
    o["auto"] = autoThreshold;
    o["softness"] = softness;
    o["invert"] = invert;
    o["showOriginal"] = showOriginal;
    o["levels"] = levels;
    o["ink"] = ink.name();
    o["paper"] = paper.name();
    o["accent"] = accent.name();
    return o;
}

AnalysisFilter AnalysisFilter::fromJson(const QJsonObject& o)
{
    AnalysisFilter f;
    f.mode = Mode(std::clamp(o.value("mode").toInt(0), 0, kModes - 1));
    const QJsonArray t = o.value("thresholds").toArray();
    for (int i = 0; i < kModes && i < t.size(); ++i) f.thresholds[size_t(i)] = std::clamp(t[i].toDouble(f.thresholds[size_t(i)]), 0.0, 1.0);
    f.autoThreshold = o.value("auto").toBool(true);
    f.softness = std::clamp(o.value("softness").toDouble(0.03), 0.0, 0.3);
    f.invert = o.value("invert").toBool();
    f.showOriginal = std::clamp(o.value("showOriginal").toDouble(0.0), 0.0, 1.0);
    f.levels = std::clamp(o.value("levels").toInt(2), 2, 3);
    auto colour = [&](const char* key, const QColor& fallback) {
        const QColor c = QColor::fromString(o.value(QLatin1String(key)).toString());
        return c.isValid() ? c : fallback;
    };
    f.ink = colour("ink", f.ink);
    f.paper = colour("paper", f.paper);
    f.accent = colour("accent", f.accent);
    return f;
}

QString AnalysisFilter::name(Mode mode)
{
    switch (mode) {
    case Mode::None: return L("No Filter", "필터 없음");
    case Mode::Silhouette: return L("Silhouette", "실루엣");
    case Mode::MovingSilhouette: return L("Moving Silhouette", "움직임 실루엣");
    case Mode::Edges: return L("Edge Lines", "윤곽선");
    case Mode::Value: return L("Value (Grayscale)", "명도 (흑백)");
    case Mode::Notan: return L("Notan (2–3 Values)", "노탄 (2–3단 명도)");
    case Mode::MotionHighlight: return L("Motion Highlight", "움직임 강조");
    }
    return {};
}

QString AnalysisFilter::description(Mode mode)
{
    switch (mode) {
    case Mode::None: return L("The picture as it is.", "원본 화면.");
    case Mode::Silhouette:
        return L("Solid shape of the subject by brightness: reads poses and lines of action without detail.",
                 "밝기 기준으로 피사체를 단색 형태로: 디테일 없이 포즈와 동작선을 읽습니다.");
    case Mode::MovingSilhouette:
        return L("Everything that differs from the shot's still background (median of the clip) becomes the shape. Best with a fixed camera.",
                 "샷의 정지 배경(클립의 중앙값)과 다른 부분만 형태로 표시합니다. 고정 카메라에 적합합니다.");
    case Mode::Edges: return L("Contours only, like a line drawing.", "윤곽만 선화처럼 표시합니다.");
    case Mode::Value: return L("Brightness only, for value and contrast checks.", "명도만 표시해 명암 대비를 확인합니다.");
    case Mode::Notan: return L("Two or three flat values: the big shapes of the picture.", "2–3단계 명도로 큰 형태를 봅니다.");
    case Mode::MotionHighlight:
        return L("What moved since the previous frame, in colour over a dimmed picture: spacing and arcs at a glance.",
                 "이전 프레임 이후 움직인 부분을 어두운 화면 위에 색으로 표시합니다: 간격과 아크를 한눈에.");
    }
    return {};
}

namespace analysis {

double luma(const QColor& c) { return lumaOf(rgbOf(c)); }

double otsu(const QImage& source)
{
    if (source.isNull()) return 0.5;
    const QImage img = (source.width() > 480 ? source.scaledToWidth(480) : source).convertToFormat(QImage::Format_RGB32);
    std::array<double, 256> hist{};
    double total = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const int v = std::clamp(int(std::lround(lumaOf(rgbAt(img, x, y)) * 255.0)), 0, 255);
            hist[size_t(v)] += 1;
            total += 1;
        }
    if (total <= 0) return 0.5;
    double sum = 0;
    for (int i = 0; i < 256; ++i) sum += i * hist[size_t(i)];
    // Every split between two well separated groups scores the same; take the middle of that
    // plateau so the threshold sits halfway between them, not right on the dark one.
    double sumB = 0, wB = 0, best = -1;
    int lo = 128, hi = 128;
    for (int i = 0; i < 256; ++i) {
        wB += hist[size_t(i)];
        sumB += i * hist[size_t(i)];
        if (wB <= 0) continue;
        const double wF = total - wB;
        if (wF <= 0) break;
        const double mB = sumB / wB, mF = (sum - sumB) / wF;
        const double between = wB * wF * (mB - mF) * (mB - mF);
        if (between > best * (1 + 1e-9) + 1e-12) {
            best = between;
            lo = hi = i;
        } else if (between >= best * (1 - 1e-9)) {
            hi = i;
        }
    }
    return ((lo + hi) / 2.0 + 0.5) / 255.0;
}

double otsu(const VideoFrame& frame)
{
    if (frame.isNull()) return 0.5;
    return otsu(frame.toImage(QSize(480, 480)));
}

QImage backgroundPlate(const QVector<QImage>& frames, int maxWidth)
{
    QVector<QImage> small;
    QSize size;
    for (const QImage& f : frames) {
        if (f.isNull()) continue;
        QImage s = f.width() > maxWidth ? f.scaledToWidth(maxWidth, Qt::SmoothTransformation) : f;
        s = s.convertToFormat(QImage::Format_RGB32);
        if (size.isEmpty()) size = s.size();
        if (s.size() != size) s = s.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        small << s;
    }
    if (small.isEmpty()) return {};
    QImage out(size, QImage::Format_RGB32);
    const int n = int(small.size());
    std::vector<int> r(static_cast<size_t>(n)), g(static_cast<size_t>(n)), b(static_cast<size_t>(n));
    for (int y = 0; y < size.height(); ++y) {
        QRgb* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            for (int i = 0; i < n; ++i) {
                const QRgb p = reinterpret_cast<const QRgb*>(small[i].constScanLine(y))[x];
                r[size_t(i)] = qRed(p);
                g[size_t(i)] = qGreen(p);
                b[size_t(i)] = qBlue(p);
            }
            const auto mid = size_t(n / 2);
            std::nth_element(r.begin(), r.begin() + mid, r.end());
            std::nth_element(g.begin(), g.begin() + mid, g.end());
            std::nth_element(b.begin(), b.begin() + mid, b.end());
            dst[x] = qRgb(r[mid], g[mid], b[mid]);
        }
    }
    return out;
}

QImage apply(const QImage& picture, const AnalysisFilter& f, double threshold, const QImage& reference)
{
    using Mode = AnalysisFilter::Mode;
    if (!f.enabled() || picture.isNull()) return picture;
    const QImage src = picture.convertToFormat(QImage::Format_RGB32);
    QImage ref;
    if (f.needsReference() && !reference.isNull())
        ref = reference.convertToFormat(QImage::Format_RGB32).scaled(src.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage out(src.size(), QImage::Format_RGB32);
    const Rgb ink = rgbOf(f.ink), paper = rgbOf(f.paper), accent = rgbOf(f.accent);
    const double soft = std::max(0.002, f.softness);
    const double t = threshold;
    for (int y = 0; y < src.height(); ++y) {
        QRgb* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            const Rgb c = rgbAt(src, x, y);
            const double Y = lumaOf(c);
            const Rgb back = mix(paper, c, f.showOriginal);
            Rgb o = c;
            switch (f.mode) {
            case Mode::None: break;
            case Mode::Silhouette: {
                const double s = smoothstep(t - soft, t + soft, Y);
                o = mix(back, ink, f.invert ? s : 1.0 - s);
                break;
            }
            case Mode::MovingSilhouette: {
                const double m = ref.isNull() ? 0.0 : smoothstep(t - soft, t + soft, difference(src, ref, x, y));
                o = mix(back, ink, m);
                break;
            }
            case Mode::Edges: {
                double l[3][3];
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i) l[j + 1][i + 1] = lumaOf(rgbAt(src, x + i, y + j));
                const double gx = (l[0][2] + 2 * l[1][2] + l[2][2]) - (l[0][0] + 2 * l[1][0] + l[2][0]);
                const double gy = (l[2][0] + 2 * l[2][1] + l[2][2]) - (l[0][0] + 2 * l[0][1] + l[0][2]);
                const double m = smoothstep(t - soft, t + soft, std::sqrt(gx * gx + gy * gy) * 0.25);
                o = mix(back, ink, m);
                break;
            }
            case Mode::Value: o = { Y, Y, Y }; break;
            case Mode::Notan: {
                const Rgb lo = f.invert ? paper : ink, hi = f.invert ? ink : paper;
                if (f.levels >= 3) o = Y < t - 0.15 ? lo : Y < t + 0.15 ? mix(lo, hi, 0.5) : hi;
                else o = Y < t ? lo : hi;
                break;
            }
            case Mode::MotionHighlight: {
                const double m = ref.isNull() ? 0.0 : smoothstep(t - soft, t + soft, difference(src, ref, x, y));
                const double base = Y * 0.55;
                o = mix(Rgb{ base, base, base }, accent, m);
                break;
            }
            }
            dst[x] = pack(o);
        }
    }
    return out;
}

} // namespace analysis
