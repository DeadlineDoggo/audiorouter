#pragma once

#include <QHash>
#include <QVector>

#include "audiotypes.h"
#include "data/audiogroup.h"

/**
 * @brief Decides which live stream each saved route applies to.
 *
 * A route is saved as "<app>\t<ordinal>" plus the media name the stream had
 * when it was wired.  Stream indices are different every time an application
 * starts playing, so the routes have to be matched to whatever streams are
 * live right now:
 *
 *  1. A stream that is already bound stays bound to its route for as long as
 *     both exist ("sticky").  Another stream of the same application ending
 *     therefore never makes the remaining streams swap outputs.
 *  2. Unbound routes first take an unbound stream of their application whose
 *     media name equals the route's hint.
 *  3. The rest are paired in order: lowest route ordinal with the oldest
 *     (lowest index) stream.
 *  4. A legacy route without ordinal takes every stream of its application
 *     that no per-stream route claimed.
 *
 * Pure logic with no dependencies on the sound server.
 */
namespace StreamBinder {

/// Live stream index → key of the route it is bound to.
using Bindings = QHash<uint32_t, QString>;

Bindings bind(const QVector<AudioRoute> &routes,
              const QVector<PASinkInputInfo> &streams,
              const Bindings &previous);

/// Lowest ordinal not used by any route of @p appName.
int freeOrdinal(const QVector<AudioRoute> &routes, const QString &appName);

} // namespace StreamBinder
