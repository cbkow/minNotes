#pragma once
// The family's path kit (minplatform crates/min-paths, canon §22) through its C ABI: a QString in, a QString out, the
// Rust allocation freed here. Every mp_* function takes UTF-8 and returns a string the caller frees with mp_free.
#include <QString>
#include <min_paths.h>

namespace mn::mp {

// Take ownership of a Rust string: copy it into a QString and free it. "" for NULL.
inline QString take(char* s) {
    if (!s) return QString();
    const QString out = QString::fromUtf8(s);
    mp_free(s);
    return out;
}
// A UTF-8 argument for one call (the QByteArray must outlive the call — hence the helper returns it, not a pointer).
inline QByteArray u8(const QString& s) { return s.toUtf8(); }

} // namespace mn::mp
