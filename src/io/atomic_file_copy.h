#pragma once

#include "io/byte_range_device.h"

namespace Licasa {

// Includes normalized paths, symlinks and hard links. The identity belongs to
// an already-open source; comparing inode/device also catches a changed alias.
bool destinationAliasesSource(const QString& destinationPath, const QString& sourcePath,
                              const ExternalFileIdentity& sourceIdentity);

// Copy an exact range from an already-open regular file, with bounded memory
// and identity checks before each read and before atomic commit. Returns an
// empty string on success. Never falls back to writing the destination in place.
QString copyFileRangeAtomically(QFile& source, const ExternalFileIdentity& identity,
                                CheckedByteRange range, const QString& destinationPath);

} // namespace Licasa
