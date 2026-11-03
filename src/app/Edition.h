#pragma once

#include <QString>

// Which build this is. The Lite edition (CHARONOS_LITE) leaves out the multi-track timeline
// editor, the motion graph and the AI / MCP integration; the project file format is the same.
namespace Edition {

#ifdef CHARONOS_LITE
constexpr bool lite = true;
#else
constexpr bool lite = false;
#endif

inline QString productName() { return lite ? QStringLiteral("CHARONOS Lite") : QStringLiteral("CHARONOS"); }
// QSettings application name: Lite keeps its own layout and preferences.
inline QString settingsName() { return lite ? QStringLiteral("CHRONOS-Lite") : QStringLiteral("CHRONOS"); }

// Releases (and the update check) live here.
inline QString repository() { return QStringLiteral("youneo25/CHARONOS"); }
inline QString releasesPage() { return QStringLiteral("https://github.com/%1/releases").arg(repository()); }

} // namespace Edition
