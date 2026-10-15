#pragma once

#include <QString>

#include <atomic>

// UI language. English is the default; Korean is chosen in Tools > Language (applies after restart).
namespace Lang {

enum Language { English = 0, Korean = 1 };

inline std::atomic<int> g_current{ English };

inline Language current() { return Language(g_current.load()); }
inline void set(Language l) { g_current = int(l); }

} // namespace Lang

// L("Open", "열기"): the text for the current UI language.
inline QString L(const char* en, const char* ko)
{
    return QString::fromUtf8(Lang::current() == Lang::Korean ? ko : en);
}
