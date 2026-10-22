#include "NoteClips.h"
#include "Lang.h"

#include <QUuid>

#include <algorithm>
#include <map>
#include <set>

namespace notes {

namespace {

struct Run {
    int first = 0, last = 0;
};

// One drawn object across the keys it appears in.
struct Obj {
    QString id;
    int order = 0;                      // first appearance (keys in time, then list order)
    std::map<int, Annotation> samples;  // key frame -> its state there
    QVector<Run> runs;                  // frames where it is visible
    QString clip;                       // clip id; "~kind@frame" until the clip is given a real id
};

struct Model {
    QVector<Obj> objs;
    QHash<QString, int> index;
    int length = 1;
};

struct Member {
    int obj = 0, run = 0;
};

struct Block {
    QString clip;
    int first = 0, last = 0;
    QVector<Member> members;
    int order = 0;
    int lane = 0;
};

QString newClipId() { return QStringLiteral("k") + QUuid::createUuid().toString(QUuid::Id128).left(10); }

int lengthOf(const Source& s, int length)
{
    int n = std::max(0, length);
    if (!s.annotations.empty()) n = std::max(n, s.annotations.rbegin()->first + 1);
    return std::max(1, n);
}

Model extract(const Source& s, int length)
{
    Model m;
    m.length = lengthOf(s, length);
    int order = 0;
    for (auto it = s.annotations.begin(); it != s.annotations.end(); ++it) {
        const int k = it->first;
        if (k < 0) continue;
        const auto next = std::next(it);
        const int end = next == s.annotations.end() ? m.length - 1 : std::min(m.length - 1, next->first - 1);
        for (const Annotation& a : it->second) {
            int i = m.index.value(a.id, -1);
            if (i < 0) {
                i = int(m.objs.size());
                m.index.insert(a.id, i);
                Obj o;
                o.id = a.id;
                o.order = order++;
                m.objs << o;
            }
            Obj& o = m.objs[i];
            o.samples[k] = a;
            if (!o.runs.isEmpty() && o.runs.back().last >= k - 1) o.runs.back().last = std::max(o.runs.back().last, end);
            else o.runs.append({ k, end });
        }
    }
    for (Obj& o : m.objs) {
        for (const auto& [f, a] : o.samples)
            if (!a.clipId.isEmpty()) {
                o.clip = a.clipId;
                break;
            }
        if (o.clip.isEmpty())
            o.clip = QStringLiteral("~%1@%2").arg(category(o.samples.begin()->second)).arg(o.runs.front().first);
    }
    return m;
}

// State of an object at a frame inside one of its runs.
const Annotation* stateAt(const Obj& o, int frame)
{
    auto it = o.samples.upper_bound(frame);
    if (it == o.samples.begin()) return nullptr;
    return &std::prev(it)->second;
}

int explicitLane(const Obj& o)
{
    for (const auto& [f, a] : o.samples)
        if (a.lane >= 0) return a.lane;
    return -1;
}

QVector<Block> blocks(const Model& m)
{
    struct Piece {
        int first, last, obj, run;
    };
    QHash<QString, QVector<Piece>> byClip;
    for (int i = 0; i < m.objs.size(); ++i)
        for (int r = 0; r < m.objs[i].runs.size(); ++r)
            byClip[m.objs[i].clip].append({ m.objs[i].runs[r].first, m.objs[i].runs[r].last, i, r });
    QVector<Block> out;
    for (auto it = byClip.begin(); it != byClip.end(); ++it) {
        QVector<Piece>& pieces = it.value();
        std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.first < b.first; });
        Block cur;
        bool open = false;
        for (const Piece& p : pieces) {
            if (open && p.first <= cur.last + 1) {
                cur.last = std::max(cur.last, p.last);
                cur.members.append({ p.obj, p.run });
                cur.order = std::min(cur.order, m.objs[p.obj].order);
                continue;
            }
            if (open) out << cur;
            cur = Block{};
            cur.clip = it.key();
            cur.first = p.first;
            cur.last = p.last;
            cur.members = { { p.obj, p.run } };
            cur.order = m.objs[p.obj].order;
            open = true;
        }
        if (open) out << cur;
    }
    std::sort(out.begin(), out.end(), [](const Block& a, const Block& b) { return a.order != b.order ? a.order < b.order : a.first < b.first; });
    return out;
}

// Gives every block a lane where it overlaps nothing. `desired` per block index (-1 = 0);
// blocks in `late` are placed after all others so the others keep their lanes.
void assignLanes(QVector<Block>& bs, const QVector<int>& desired, const QSet<int>& late = {})
{
    QVector<QVector<Run>> taken;
    auto freeAt = [&](int lane, const Block& b) {
        if (lane >= taken.size()) return true;
        for (const Run& r : taken[lane])
            if (r.first <= b.last && b.first <= r.last) return false;
        return true;
    };
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < bs.size(); ++i) {
            if (late.contains(i) != (pass == 1)) continue;
            int lane = std::max(0, desired.value(i, -1));
            while (!freeAt(lane, bs[i])) ++lane;
            while (taken.size() <= lane) taken.append(QVector<Run>());
            taken[lane].append({ bs[i].first, bs[i].last });
            bs[i].lane = lane;
        }
}

QVector<Block> laidOut(const Model& m)
{
    QVector<Block> bs = blocks(m);
    QVector<int> desired;
    for (const Block& b : bs) {
        int lane = -1;
        for (const Member& mb : b.members) lane = std::max(lane, explicitLane(m.objs[mb.obj]));
        desired << lane;
    }
    assignLanes(bs, desired);
    return bs;
}

int findBlock(const QVector<Block>& bs, const QString& key)
{
    for (int i = 0; i < bs.size(); ++i)
        if (bs[i].clip + QLatin1Char('@') + QString::number(bs[i].first) == key) return i;
    return -1;
}

void normalizeRuns(Obj& o)
{
    std::sort(o.runs.begin(), o.runs.end(), [](const Run& a, const Run& b) { return a.first < b.first; });
    QVector<Run> merged;
    for (const Run& r : o.runs) {
        if (r.last < r.first) continue;
        if (!merged.isEmpty() && r.first <= merged.back().last + 1) merged.back().last = std::max(merged.back().last, r.last);
        else merged << r;
    }
    o.runs = merged;
    // Samples outside every run are dead; each run starts with a sample (its state there).
    for (auto it = o.samples.begin(); it != o.samples.end();) {
        const bool inside = std::any_of(o.runs.begin(), o.runs.end(), [&](const Run& r) { return it->first >= r.first && it->first <= r.last; });
        it = inside ? std::next(it) : o.samples.erase(it);
    }
}

bool sameList(const AnnotationList& a, const AnnotationList& b)
{
    if (a.size() != b.size()) return false;
    for (qsizetype i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].toJson() != b[i].toJson()) return false;
    return true;
}

// Writes the model back as keys: a key wherever what is visible changes.
void rebuild(Source& s, const Model& m)
{
    const std::map<int, AnnotationList> original = s.annotations;
    std::set<int> bounds;
    for (const Obj& o : m.objs) {
        for (const Run& r : o.runs) {
            bounds.insert(r.first);
            if (r.last + 1 < m.length) bounds.insert(r.last + 1);
        }
        for (const auto& [f, a] : o.samples) bounds.insert(f);
    }
    std::map<int, AnnotationList> out;
    AnnotationList prev;
    for (int b : bounds) {
        if (b < 0 || b >= m.length) continue;
        struct Vis {
            const Obj* obj;
            const Annotation* state;
            int rank;
        };
        QHash<QString, int> rank;
        if (auto it = original.upper_bound(b); it != original.begin()) {
            int i = 0;
            for (const Annotation& a : std::prev(it)->second) rank.insert(a.id, i++);
        }
        QVector<Vis> vis;
        for (const Obj& o : m.objs)
            for (const Run& r : o.runs)
                if (r.first <= b && b <= r.last) {
                    if (const Annotation* st = stateAt(o, b)) vis.append({ &o, st, rank.value(o.id, 1000000 + o.order) });
                    break;
                }
        std::stable_sort(vis.begin(), vis.end(), [](const Vis& x, const Vis& y) {
            const int lx = std::max(0, x.state->lane), ly = std::max(0, y.state->lane);
            return lx != ly ? lx < ly : x.rank < y.rank;
        });
        AnnotationList list;
        for (const Vis& v : vis) list << *v.state;
        if (sameList(list, prev)) continue;
        out[b] = list;
        prev = list;
    }
    s.annotations = std::move(out);
}

// Settles lanes (others keep theirs; `moved` objects go to `movedLane` or the nearest free lane
// above), gives virtual clips real ids, then rewrites the keys.
void finish(Source& s, Model& m, const QHash<QString, int>& before, const QSet<QString>& moved = {}, int movedLane = -1)
{
    for (Obj& o : m.objs) normalizeRuns(o);
    m.objs.removeIf([](const Obj& o) { return o.runs.isEmpty(); });
    m.index.clear();
    for (int i = 0; i < m.objs.size(); ++i) m.index.insert(m.objs[i].id, i);

    QVector<Block> bs = blocks(m);
    QVector<int> desired;
    QSet<int> late;
    for (int i = 0; i < bs.size(); ++i) {
        int lane = -1;
        bool isMoved = false;
        for (const Member& mb : bs[i].members) {
            const Obj& o = m.objs[mb.obj];
            if (moved.contains(o.id)) isMoved = true;
            lane = std::max(lane, before.value(o.id, explicitLane(o)));
        }
        if (isMoved) {
            late.insert(i);
            if (movedLane >= 0) lane = movedLane;
        }
        desired << lane;
    }
    assignLanes(bs, desired, late);
    QHash<QString, QString> realIds;
    for (const Block& b : bs)
        for (const Member& mb : b.members) {
            Obj& o = m.objs[mb.obj];
            if (o.clip.startsWith(QLatin1Char('~'))) {
                if (!realIds.contains(o.clip)) realIds.insert(o.clip, newClipId());
                o.clip = realIds.value(o.clip);
            }
            for (auto& [f, a] : o.samples) {
                a.lane = b.lane;
                a.clipId = o.clip;
            }
        }
    rebuild(s, m);
}

QHash<QString, int> lanesOf(const Model& m, const QVector<Block>& bs)
{
    QHash<QString, int> out;
    for (const Block& b : bs)
        for (const Member& mb : b.members) out.insert(m.objs[mb.obj].id, b.lane);
    return out;
}

QString iconFor(const Annotation& a)
{
    if (a.type == "text") return QStringLiteral("text");
    if (a.type == "stroke") return a.penStyle == "pen" ? QStringLiteral("pen") : a.penStyle == "highlighter" ? QStringLiteral("highlighter") : QStringLiteral("pencil");
    if (a.type == "image") return QStringLiteral("image");
    if (a.type == "circle") return QStringLiteral("circle");
    if (a.type == "arrow") return QStringLiteral("arrow");
    if (a.type == "line") return QStringLiteral("line");
    return QStringLiteral("rect");
}

// Objects of a block restricted to that block's runs, as a model of their own (for copies).
Obj copyRun(const Obj& o, const Run& r, const QString& clip, int shift)
{
    Obj c;
    c.id = Annotation::newId();
    c.order = o.order;
    c.clip = clip;
    if (const Annotation* st = stateAt(o, r.first)) {
        Annotation a = *st;
        a.id = c.id;
        a.clipId = clip;
        c.samples[r.first + shift] = a;
    }
    for (const auto& [f, a] : o.samples)
        if (f > r.first && f <= r.last) {
            Annotation x = a;
            x.id = c.id;
            x.clipId = clip;
            c.samples[f + shift] = x;
        }
    c.runs = { { r.first + shift, r.last + shift } };
    return c;
}

} // namespace

QString category(const Annotation& a)
{
    if (a.type == "text") return QStringLiteral("text");
    if (a.type == "stroke") return QStringLiteral("draw");
    if (a.type == "image") return QStringLiteral("image");
    return QStringLiteral("shape");
}

QVector<NoteClip> clips(const Source& s, int length)
{
    QVector<NoteClip> out;
    if (s.annotations.empty()) return out;
    const Model m = extract(s, length);
    const QVector<Block> bs = laidOut(m);
    for (const Block& b : bs) {
        NoteClip n;
        n.clipId = b.clip;
        n.key = b.clip + QLatin1Char('@') + QString::number(b.first);
        n.first = b.first;
        n.last = b.last;
        n.lane = b.lane;
        QSet<QString> kinds, icons;
        int strokes = 0, shapes = 0;
        bool allLocked = true;
        const Annotation* lead = nullptr;
        for (const Member& mb : b.members) {
            const Obj& o = m.objs[mb.obj];
            const Annotation* st = stateAt(o, o.runs[mb.run].first);
            if (!st) continue;
            if (!lead) lead = st;
            if (!n.ids.contains(o.id)) n.ids << o.id;
            kinds.insert(category(*st));
            icons.insert(iconFor(*st));
            strokes += st->type == "stroke";
            shapes += category(*st) == "shape";
            n.tracked = n.tracked || !st->trackId.isEmpty();
            allLocked = allLocked && st->locked;
            if (st->type == "text" && n.label.isEmpty()) n.label = st->text.section(QLatin1Char('\n'), 0, 0).trimmed();
            if (!n.color.isValid() && st->clipColor.isValid()) n.color = st->clipColor;
        }
        if (!lead) continue;
        n.locked = allLocked;
        n.kind = kinds.size() == 1 ? *kinds.begin() : QStringLiteral("mixed");
        n.icon = icons.size() == 1 ? *icons.begin() : n.kind == "draw" ? QStringLiteral("pencil") : n.kind == "shape" ? QStringLiteral("rect") : QStringLiteral("layers");
        if (!n.color.isValid()) n.color = lead->color;
        if (n.label.isEmpty()) {
            if (n.kind == "draw") n.label = strokes == 1 ? L("Drawing", "드로잉") : L("Drawing · %1 strokes", "드로잉 · %1획").arg(strokes);
            else if (n.kind == "shape") n.label = shapes == 1 ? L("Shape", "도형") : L("%1 shapes", "도형 %1개").arg(shapes);
            else if (n.kind == "image") n.label = L("Image", "이미지");
            else n.label = L("%1 objects", "객체 %1개").arg(n.ids.size());
        }
        out << n;
    }
    std::sort(out.begin(), out.end(), [](const NoteClip& a, const NoteClip& b) { return a.lane != b.lane ? a.lane < b.lane : a.first < b.first; });
    return out;
}

QHash<QString, int> lanes(const Source& s, int length)
{
    if (s.annotations.empty()) return {};
    const Model m = extract(s, length);
    return lanesOf(m, laidOut(m));
}

void placeNew(const Source& s, int length, int frame, Annotation& a, bool keepOthers)
{
    const Model m = extract(s, length);
    const QVector<Block> bs = laidOut(m);
    const QString cat = category(a);
    // Same kind drawn at this very key: the same clip.
    for (const Block& b : bs) {
        if (b.first != frame) continue;
        for (const Member& mb : b.members) {
            const Obj& o = m.objs[mb.obj];
            const Annotation* st = stateAt(o, frame);
            if (!st || category(*st) != cat || o.runs[mb.run].first != frame) continue;
            if (!o.clip.startsWith(QLatin1Char('~'))) a.clipId = o.clip;
            a.lane = b.lane;
            if (st->clipColor.isValid()) a.clipColor = st->clipColor;
            return;
        }
    }
    if (!keepOthers) {
        a.lane = 0;
        return;
    }
    // Otherwise on top of whatever stays visible with it (it is drawn last, so it shows on top).
    auto next = s.annotations.upper_bound(frame);
    const int end = next == s.annotations.end() ? m.length - 1 : next->first - 1;
    int lane = -1;
    for (const Block& b : bs)
        if (b.first <= end && frame <= b.last) {
            // Blocks that stop at this frame (a new key ends them) don't count.
            bool continues = false;
            for (const Member& mb : b.members) {
                const Run& r = m.objs[mb.obj].runs[mb.run];
                if (r.first <= frame && frame <= r.last) continues = true;
            }
            if (continues) lane = std::max(lane, b.lane);
        }
    a.lane = lane + 1;
}

bool move(Source& s, int length, const QString& key, int newFirst, int newLane)
{
    Model m = extract(s, length);
    const QVector<Block> bs = laidOut(m);
    const int bi = findBlock(bs, key);
    if (bi < 0) return false;
    const Block& b = bs[bi];
    // Later than the source allows: the clip loses its tail at the source's last frame.
    const int d = std::clamp(newFirst - b.first, -b.first, m.length - 1 - b.first);
    const int lane = newLane >= 0 ? newLane : b.lane;
    if (d == 0 && lane == b.lane) return false;
    const QHash<QString, int> before = lanesOf(m, bs);
    QSet<QString> moved;
    for (const Member& mb : b.members) {
        Obj& o = m.objs[mb.obj];
        Run& r = o.runs[mb.run];
        moved.insert(o.id);
        if (d == 0) continue;
        std::map<int, Annotation> shifted;
        for (auto it = o.samples.begin(); it != o.samples.end();) {
            if (it->first >= r.first && it->first <= r.last) {
                shifted[it->first + d] = it->second;
                it = o.samples.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& [f, a] : shifted) o.samples[f] = a;
        r.first += d;
        r.last = std::min(r.last + d, m.length - 1);
    }
    finish(s, m, before, moved, lane);
    return true;
}

bool trim(Source& s, int length, const QString& key, int newFirst, int newLast)
{
    Model m = extract(s, length);
    const QVector<Block> bs = laidOut(m);
    const int bi = findBlock(bs, key);
    if (bi < 0) return false;
    const Block& b = bs[bi];
    newFirst = std::clamp(newFirst, 0, m.length - 1);
    newLast = std::clamp(newLast, newFirst, m.length - 1);
    if (newFirst == b.first && newLast == b.last) return false;
    const QHash<QString, int> before = lanesOf(m, bs);
    QSet<QString> moved;
    for (const Member& mb : b.members) {
        Obj& o = m.objs[mb.obj];
        Run& r = o.runs[mb.run];
        moved.insert(o.id);
        // head
        if (newFirst > r.first) {
            if (newFirst > r.last) {
                r.last = r.first - 1;  // dropped
                continue;
            }
            const Annotation carry = *stateAt(o, newFirst);
            for (auto it = o.samples.begin(); it != o.samples.end();)
                it = (it->first >= r.first && it->first <= newFirst) ? o.samples.erase(it) : std::next(it);
            o.samples[newFirst] = carry;
            r.first = newFirst;
        } else if (newFirst < r.first && r.first == b.first) {
            const Annotation carry = o.samples[r.first];
            o.samples.erase(r.first);
            o.samples[newFirst] = carry;
            r.first = newFirst;
        }
        // tail
        if (newLast < r.last) {
            if (newLast < r.first) {
                r.last = r.first - 1;
                continue;
            }
            for (auto it = o.samples.begin(); it != o.samples.end();)
                it = (it->first > newLast && it->first <= r.last) ? o.samples.erase(it) : std::next(it);
            r.last = newLast;
        } else if (newLast > r.last && r.last == b.last) {
            r.last = newLast;
        }
    }
    finish(s, m, before, moved, b.lane);
    return true;
}

bool split(Source& s, int length, const QString& key, int frame, QString* newKey)
{
    Model m = extract(s, length);
    const QVector<Block> bs = laidOut(m);
    const int bi = findBlock(bs, key);
    if (bi < 0) return false;
    const Block& b = bs[bi];
    if (frame <= b.first || frame > b.last) return false;
    const QHash<QString, int> before = lanesOf(m, bs);
    const QString clip = newClipId();
    QVector<Obj> added;
    for (const Member& mb : b.members) {
        Obj& o = m.objs[mb.obj];
        Run& r = o.runs[mb.run];
        if (r.last < frame) continue;
        const Run tail{ std::max(r.first, frame), r.last };
        Obj c = copyRun(o, tail, clip, 0);
        for (auto& [f, a] : c.samples) a.lane = b.lane;
        added << c;
        for (auto it = o.samples.begin(); it != o.samples.end();)
            it = (it->first >= tail.first && it->first <= r.last) ? o.samples.erase(it) : std::next(it);
        r.last = tail.first - 1;  // nothing left when the whole run moved
    }
    if (added.isEmpty()) return false;
    for (const Obj& o : added) m.objs << o;
    QHash<QString, int> lanes = before;
    for (const Obj& o : added) lanes.insert(o.id, b.lane);
    finish(s, m, lanes);
    if (newKey) *newKey = clip + QLatin1Char('@') + QString::number(frame);
    return true;
}

bool erase(Source& s, int length, const QStringList& keys)
{
    Model m = extract(s, length);
    const QVector<Block> bs = laidOut(m);
    const QHash<QString, int> before = lanesOf(m, bs);
    bool any = false;
    for (const QString& key : keys) {
        const int bi = findBlock(bs, key);
        if (bi < 0) continue;
        any = true;
        for (const Member& mb : bs[bi].members) {
            Obj& o = m.objs[mb.obj];
            Run& r = o.runs[mb.run];
            for (auto it = o.samples.begin(); it != o.samples.end();)
                it = (it->first >= r.first && it->first <= r.last) ? o.samples.erase(it) : std::next(it);
            r.last = r.first - 1;
        }
    }
    if (!any) return false;
    finish(s, m, before);
    return true;
}

bool duplicate(Source& s, int length, const QString& key, int newFirst, QString* newKey)
{
    Model m = extract(s, length);
    const QVector<Block> bs = laidOut(m);
    const int bi = findBlock(bs, key);
    if (bi < 0) return false;
    const Block& b = bs[bi];
    const int d = std::clamp(newFirst - b.first, -b.first, m.length - 1 - b.first);
    const QHash<QString, int> before = lanesOf(m, bs);
    const QString clip = newClipId();
    QSet<QString> moved;
    QVector<Obj> added;
    for (const Member& mb : b.members) {
        const Obj& o = m.objs[mb.obj];
        Obj c = copyRun(o, o.runs[mb.run], clip, d);
        for (Run& r : c.runs) r.last = std::min(r.last, m.length - 1);
        moved.insert(c.id);
        added << c;
    }
    for (const Obj& o : added) m.objs << o;
    finish(s, m, before, moved, b.lane);
    if (newKey) *newKey = clip + QLatin1Char('@') + QString::number(b.first + d);
    return true;
}

bool setColor(Source& s, int length, const QString& key, const QColor& label, bool recolourObjects)
{
    Model m = extract(s, length);
    const QVector<Block> bs = laidOut(m);
    const int bi = findBlock(bs, key);
    if (bi < 0) return false;
    const QHash<QString, int> before = lanesOf(m, bs);
    for (const Member& mb : bs[bi].members) {
        Obj& o = m.objs[mb.obj];
        const Run& r = o.runs[mb.run];
        for (auto& [f, a] : o.samples)
            if (f >= r.first && f <= r.last) {
                a.clipColor = label;
                if (recolourObjects && label.isValid()) a.color = label;
            }
    }
    finish(s, m, before);
    return true;
}

bool setLocked(Source& s, int length, const QString& key, bool locked)
{
    Model m = extract(s, length);
    const QVector<Block> bs = laidOut(m);
    const int bi = findBlock(bs, key);
    if (bi < 0) return false;
    const QHash<QString, int> before = lanesOf(m, bs);
    for (const Member& mb : bs[bi].members) {
        Obj& o = m.objs[mb.obj];
        const Run& r = o.runs[mb.run];
        for (auto& [f, a] : o.samples)
            if (f >= r.first && f <= r.last) a.locked = locked;
    }
    finish(s, m, before);
    return true;
}

} // namespace notes
