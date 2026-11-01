#include "io/atomic_file_copy.h"

#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <array>

namespace Licasa {

bool destinationAliasesSource(const QString& destinationPath, const QString& sourcePath,
                              const ExternalFileIdentity& sourceIdentity)
{
    if (QDir::cleanPath(QFileInfo(destinationPath).absoluteFilePath()) ==
        QDir::cleanPath(QFileInfo(sourcePath).absoluteFilePath())) {
        return true;
    }

    QFile destination(destinationPath);
    if (!destination.open(QIODevice::ReadOnly)) {
        return false; // QSaveFile supplies the authoritative destination error.
    }
    const auto identity = externalFileIdentity(destination);
    return identity && identity->device == sourceIdentity.device &&
           identity->inode == sourceIdentity.inode;
}

QString copyFileRangeAtomically(QFile& source, const ExternalFileIdentity& identity,
                                CheckedByteRange range, const QString& destinationPath)
{
    const auto unchanged = [&]() {
        const auto current = externalFileIdentity(source);
        return current && *current == identity;
    };
    const QString changedError =
        QCoreApplication::translate("AtomicFileCopy", "The source file changed during export.");
    if (!unchanged()) {
        return changedError;
    }
    if (!checkedByteRange(identity.size, range.offset, range.length)) {
        return QCoreApplication::translate("AtomicFileCopy", "The export range no longer fits the source file.");
    }
    if (destinationAliasesSource(destinationPath, source.fileName(), identity)) {
        return QCoreApplication::translate(
            "AtomicFileCopy", "Choose a destination that is not an alias of the source file.");
    }
    // A regular QFile's signed size and the range check bound this conversion.
    if (!source.seek(qint64(range.offset))) {
        return QCoreApplication::translate("AtomicFileCopy", "Unable to seek the export source.");
    }

    QSaveFile output(destinationPath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        return output.errorString();
    }

    std::array<char, 256 * 1024> buffer;
    quint64 remaining = range.length;
    while (remaining > 0) {
        const qint64 request = qint64(std::min<quint64>(remaining, buffer.size()));
        if (!unchanged() || source.read(buffer.data(), request) != request) {
            output.cancelWriting();
            return changedError;
        }
        if (output.write(buffer.data(), request) != request) {
            const QString error = output.errorString();
            output.cancelWriting();
            return error.isEmpty()
                       ? QCoreApplication::translate("AtomicFileCopy", "Unable to write the export.")
                       : error;
        }
        remaining -= quint64(request);
    }
    if (!unchanged()) {
        output.cancelWriting();
        return changedError;
    }
    if (!output.commit()) {
        return output.errorString();
    }
    return {};
}

} // namespace Licasa
