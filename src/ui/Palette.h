#pragma once

#include <QColor>

// Warm-neutral dark palette. One muted accent, used only for the playhead,
// in/out range and focus; everything else is greyscale.
namespace Pal {

inline const QColor window{ 0x1f, 0x1e, 0x1d };     // app chrome
inline const QColor panel{ 0x26, 0x26, 0x24 };      // side panels
inline const QColor raised{ 0x30, 0x30, 0x2e };     // inputs, hovered rows
inline const QColor hover{ 0x38, 0x37, 0x35 };
inline const QColor selected{ 0x40, 0x3f, 0x3c };
inline const QColor border{ 0x3a, 0x39, 0x36 };
inline const QColor divider{ 0x2c, 0x2b, 0x29 };
inline const QColor viewer{ 0x12, 0x12, 0x11 };     // behind the image
inline const QColor text{ 0xe5, 0xe3, 0xdb };
inline const QColor textStrong{ 0xfa, 0xf9, 0xf5 };
inline const QColor textDim{ 0xa3, 0xa0, 0x97 };
inline const QColor textFaint{ 0x6e, 0x6c, 0x66 };
inline const QColor iconIdle{ 0xb4, 0xb1, 0xa8 };
inline const QColor accent{ 0xd9, 0x77, 0x57 };     // clay
inline const QColor accentSoft{ 0xd9, 0x77, 0x57, 60 };
inline const QColor cached{ 0x5a, 0x58, 0x53 };
inline const QColor marker{ 0xc9, 0xb3, 0x8a };     // annotation ticks (muted sand)
inline const QColor review{ 0xe7, 0x6c, 0x70 };     // red review key ticks
inline const QColor reviewActive{ 0xa6, 0xdc, 0xb5 }; // soft mint frame text

} // namespace Pal
