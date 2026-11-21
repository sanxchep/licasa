#include "io/atomic_file_copy.h"

#include <QCoreApplication>
#include <QDir>
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

#if defined(Q_OS_UNIX)
    const auto identity = externalFileIdentityAtPath(destinationPath);
    return identity && identity->device == sourceIdentity.device &&
           identity->inode == sourceIdentity.inode;
#else
    Q_UNUSED(sourceIdentity);
    return false;
#endif
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
        return QCoreApplication::translate("AtomicFileCopy",
                                           "The export range no longer fits the source file.");
    }
    const auto aliasesSource = [&]() {
        return destinationAliasesSource(destinationPath, source.fileName(), identity);
    };
    const auto aliasError = []() {
        return QCoreApplication::translate(
            "AtomicFileCopy", "Choose a destination that is not an alias of the source file.");
    };
    if (aliasesSource()) {
        return aliasError();
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
            return error.isEmpty() ? QCoreApplication::translate("AtomicFileCopy",
                                                                 "Unable to write the export.")
                                   : error;
        }
        remaining -= quint64(request);
    }
    if (!unchanged()
#if defined(Q_OS_UNIX)
        || externalFileIdentityAtPath(source.fileName()) != identity
#endif
    ) {
        output.cancelWriting();
        return changedError;
    }
    if (aliasesSource()) {
        output.cancelWriting();
        return aliasError();
    }
    if (!output.commit()) {
        return output.errorString();
    }
    return {};
}

} // namespace Licasa
