#pragma once

#include <QString>

namespace Licasa {

// Returns an absolute endpoint inside the current user's protected runtime
// directory. An empty value deliberately disables single-instance IPC.
QString singleInstanceEndpointName();

namespace Internal {

// Exposed only so the platform boundary can be regression-tested without
// creating an application server.
bool isPrivateSingleInstanceRuntimeDirectory(const QString& directory);

} // namespace Internal
} // namespace Licasa
