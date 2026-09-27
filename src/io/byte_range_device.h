#pragma once

#include "io/external_file_identity.h"

#include <QFile>
#include <QIODevice>
#include <QString>

#include <atomic>
#include <optional>

namespace Licasa {

struct CheckedByteRange {
    quint64 offset = 0;
    quint64 length = 0;
};

// Metadata-facing helpers use unsigned arithmetic because container offsets and
// lengths are commonly encoded as unsigned integers. Validation is expressed
// with subtraction rather than offset + length, so malformed metadata cannot
// wrap before the actual file-size bound is checked.
std::optional<quint64> checkedByteOffsetAdd(quint64 left, quint64 right) noexcept;
std::optional<CheckedByteRange> checkedByteRange(quint64 fileSize, quint64 offset,
                                                 quint64 length) noexcept;

// Read-only, seekable view over one validated region of a source file. The
// object owns its QFile so the source descriptor remains alive for the entire
// consumer lifetime and is not affected by unrelated QIODevice seeks.
class ByteRangeDevice final : public QIODevice {
  public:
    ByteRangeDevice(QString sourcePath, quint64 offset, quint64 length,
                    const std::atomic_bool* cancelled = nullptr, QObject* parent = nullptr);
    ByteRangeDevice(QString sourcePath, quint64 offset, quint64 length,
                    ExternalFileIdentity expectedIdentity,
                    const std::atomic_bool* cancelled = nullptr, QObject* parent = nullptr);
    ~ByteRangeDevice() override;

    bool open(OpenMode mode) override;
    void close() override;

    bool isSequential() const override { return false; }
    qint64 size() const override { return validatedLength_; }
    bool seek(qint64 position) override;

  protected:
    qint64 readData(char* data, qint64 maximumSize) override;
    qint64 writeData(const char*, qint64) override;

  private:
    bool cancelled() const noexcept;
    bool validateCurrentSource(QString* reason = nullptr) const;
    void fail(const QString& reason);

    quint64 declaredOffset_ = 0;
    quint64 declaredLength_ = 0;
    const std::atomic_bool* cancelled_ = nullptr;
    QFile source_;
    qint64 validatedOffset_ = -1;
    qint64 validatedLength_ = -1;
    std::optional<ExternalFileIdentity> expectedIdentity_;
};

} // namespace Licasa
