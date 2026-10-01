#include "io/byte_range_device.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Licasa {

std::optional<quint64> checkedByteOffsetAdd(quint64 left, quint64 right) noexcept
{
    if (right > std::numeric_limits<quint64>::max() - left) {
        return std::nullopt;
    }
    return left + right;
}

std::optional<CheckedByteRange> checkedByteRange(quint64 fileSize, quint64 offset,
                                                 quint64 length) noexcept
{
    if (offset > fileSize || length > fileSize - offset) {
        return std::nullopt;
    }
    return CheckedByteRange{offset, length};
}

ByteRangeDevice::ByteRangeDevice(QString sourcePath, quint64 offset, quint64 length,
                                 const std::atomic_bool* cancelled, QObject* parent)
    : QIODevice(parent), declaredOffset_(offset), declaredLength_(length), cancelled_(cancelled),
      source_(std::move(sourcePath))
{}

ByteRangeDevice::ByteRangeDevice(QString sourcePath, quint64 offset, quint64 length,
                                 ExternalFileIdentity expectedIdentity,
                                 const std::atomic_bool* cancelled, QObject* parent)
    : QIODevice(parent), declaredOffset_(offset), declaredLength_(length), cancelled_(cancelled),
      source_(std::move(sourcePath)), expectedIdentity_(expectedIdentity)
{}

ByteRangeDevice::~ByteRangeDevice() { close(); }

bool ByteRangeDevice::open(OpenMode mode)
{
    if (isOpen()) {
        fail(QStringLiteral("Byte range is already open"));
        return false;
    }
    if (mode != QIODevice::ReadOnly) {
        fail(QStringLiteral("Byte range is read-only"));
        return false;
    }
    if (cancelled()) {
        fail(QStringLiteral("Byte-range operation cancelled"));
        return false;
    }
    if (declaredOffset_ > quint64(std::numeric_limits<qint64>::max()) ||
        declaredLength_ > quint64(std::numeric_limits<qint64>::max())) {
        fail(QStringLiteral("Byte range exceeds seekable file limits"));
        return false;
    }
    if (!source_.open(QIODevice::ReadOnly)) {
        fail(source_.errorString());
        return false;
    }
    const qint64 signedSize = source_.size();
    if (signedSize < 0 ||
        !checkedByteRange(quint64(signedSize), declaredOffset_, declaredLength_)) {
        fail(QStringLiteral("Byte range lies outside the source file"));
        source_.close();
        return false;
    }

    validatedOffset_ = qint64(declaredOffset_);
    validatedLength_ = qint64(declaredLength_);
    QString identityReason;
    if (!validateCurrentSource(&identityReason)) {
        fail(identityReason);
        source_.close();
        validatedOffset_ = -1;
        validatedLength_ = -1;
        return false;
    }
    // The byte-range contract must observe each caller read directly. QIODevice's
    // default buffering may otherwise prefetch past the requested amount, which
    // would make cancellation/truncation checks happen too early and allow later
    // reads to be served without revalidation.
    if (!QIODevice::open(mode | QIODevice::Unbuffered)) {
        source_.close();
        validatedOffset_ = -1;
        validatedLength_ = -1;
        return false;
    }
    return true;
}

void ByteRangeDevice::close()
{
    if (source_.isOpen()) {
        source_.close();
    }
    validatedOffset_ = -1;
    validatedLength_ = -1;
    QIODevice::close();
}

bool ByteRangeDevice::seek(qint64 position)
{
    if (!isOpen() || cancelled()) {
        if (cancelled()) {
            fail(QStringLiteral("Byte-range operation cancelled"));
        }
        return false;
    }
    if (position < 0 || position > validatedLength_) {
        fail(QStringLiteral("Seek lies outside the byte range"));
        return false;
    }
    QString reason;
    if (!validateCurrentSource(&reason)) {
        fail(reason);
        return false;
    }
    if (!QIODevice::seek(position)) {
        fail(QStringLiteral("Unable to seek within byte range"));
        return false;
    }
    return true;
}

qint64 ByteRangeDevice::readData(char* data, qint64 maximumSize)
{
    if (!isOpen() || maximumSize < 0) {
        fail(QStringLiteral("Invalid byte-range read"));
        return -1;
    }
    const qint64 logicalPosition = QIODevice::pos();
    if (logicalPosition < 0 || logicalPosition > validatedLength_) {
        fail(QStringLiteral("Byte-range position is invalid"));
        return -1;
    }
    if (maximumSize == 0 || logicalPosition == validatedLength_) {
        return 0;
    }
    if (!data) {
        fail(QStringLiteral("Invalid byte-range read buffer"));
        return -1;
    }
    if (cancelled()) {
        fail(QStringLiteral("Byte-range operation cancelled"));
        return -1;
    }

    QString reason;
    if (!validateCurrentSource(&reason)) {
        fail(reason);
        return -1;
    }

    const qint64 remaining = validatedLength_ - logicalPosition;
    const qint64 requested = std::min(maximumSize, remaining);

    // Both terms are non-negative and validatedOffset_ + validatedLength_ was
    // bounded by QFile::size() at open and again above. This subtraction-based
    // invariant guarantees the addition is representable in qint64.
    if (validatedOffset_ > std::numeric_limits<qint64>::max() - logicalPosition) {
        fail(QStringLiteral("Byte-range offset overflow"));
        return -1;
    }
    const qint64 sourcePosition = validatedOffset_ + logicalPosition;
    if (!source_.seek(sourcePosition)) {
        fail(source_.errorString());
        return -1;
    }

    const qint64 received = source_.read(data, requested);
    if (received != requested) {
        fail(received < 0 ? source_.errorString()
                          : QStringLiteral("Source file changed during byte-range read"));
        return -1;
    }

    return received;
}

qint64 ByteRangeDevice::writeData(const char*, qint64)
{
    fail(QStringLiteral("Byte range is read-only"));
    return -1;
}

bool ByteRangeDevice::cancelled() const noexcept
{
    return cancelled_ && cancelled_->load(std::memory_order_relaxed);
}

bool ByteRangeDevice::validateCurrentSource(QString* reason) const
{
    if (!source_.isOpen() || !source_.isReadable()) {
        if (reason) {
            *reason = QStringLiteral("Source file is not readable");
        }
        return false;
    }
    const qint64 signedSize = source_.size();
    if (signedSize < 0 || validatedOffset_ < 0 || validatedLength_ < 0 ||
        !checkedByteRange(quint64(signedSize), quint64(validatedOffset_),
                          quint64(validatedLength_))) {
        if (reason) {
            *reason = QStringLiteral("Source file was truncated below the byte range");
        }
        return false;
    }
    if (expectedIdentity_) {
        QString identityReason;
        const auto current = externalFileIdentity(source_, &identityReason);
        if (!current) {
            if (reason) {
                *reason = identityReason;
            }
            return false;
        }
        if (*current != *expectedIdentity_) {
            if (reason) {
                *reason = QStringLiteral(
                    "External video file changed after Live Photo pairing validation");
            }
            return false;
        }
    }
    return true;
}

void ByteRangeDevice::fail(const QString& reason) { setErrorString(reason); }

} // namespace Licasa
