#pragma once

#include "Project.h"

#include <QHash>

// Annotation clips (1.1). Annotations are stored the 1.0 way, as keys per source frame (a key
// holds until the next one), so files stay compatible. Seen as clips, each drawn object is
// visible over runs of frames; objects drawn together (same clip id) form one block on an
// annotation lane, like a title clip in an editing program. Editing a block (move, trim, split,
// duplicate, delete, lane, colour) rewrites the keys so the picture matches the blocks exactly.
struct NoteClip {
    QString key;       // clip id + '@' + first frame (unique per source)
    QString clipId;
    int first = 0;     // source frames, inclusive
    int last = 0;
    int lane = 0;      // annotation track (0 = right above the video; higher draws on top)
    QString kind;      // text | draw | shape | image | mixed
    QString icon;      // icon name: text, pencil, pen, highlighter, rect, circle, arrow, line, image
    QColor color;      // block colour (label colour or the objects' own colour)
    QString label;     // text content or a short description
    QStringList ids;   // objects in the block
    bool tracked = false;  // follows a tracking marker
    bool locked = false;   // every object is locked
};

namespace notes {

QString category(const Annotation& a);

// Blocks of a source, ordered by lane then time. `length` = source frames (keys hold to the end).
QVector<NoteClip> clips(const Source& s, int length);
// Display lane of every object visible somewhere in the source (for hiding lanes).
QHash<QString, int> lanes(const Source& s, int length);

// New object drawn at `frame`: joins the clip of the same kind drawn at that key, or starts a
// clip on the lane above whatever stays visible with it. `keepOthers` = the objects shown at
// that frame stay (continue drawing, or the frame already is a key); otherwise the new key
// replaces them and the object starts on the first lane.
void placeNew(const Source& s, int length, int frame, Annotation& a, bool keepOthers = true);

// ---- edits (true when the source changed)
bool move(Source& s, int length, const QString& key, int newFirst, int newLane);
bool trim(Source& s, int length, const QString& key, int newFirst, int newLast);
bool split(Source& s, int length, const QString& key, int frame, QString* newKey = nullptr);
bool erase(Source& s, int length, const QStringList& keys);
bool duplicate(Source& s, int length, const QString& key, int newFirst, QString* newKey = nullptr);
bool setColor(Source& s, int length, const QString& key, const QColor& label, bool recolourObjects);
bool setLocked(Source& s, int length, const QString& key, bool locked);

} // namespace notes
