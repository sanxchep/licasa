#include "imaging/fast_svgz_input.h"

#include <QFile>
#include <QtEndian>
#include <libdeflate.h>

#include <algorithm>
#include <memory>

namespace Licasa {
QByteArray readFastSvgzInput(const QString& path, quint64 maximumBytes,
                             const std::atomic_bool* cancelled)
{
    const auto stopped = [cancelled] {
        return cancelled && cancelled->load(std::memory_order_relaxed);
    };
    constexpr quint64 maximumInputBytes = 512ULL * 1024 * 1024;
    constexpr quint64 maximumOutputBytes = 512ULL * 1024 * 1024;
    QFile file(path);
    if (stopped() || maximumBytes == 0 || !file.open(QIODevice::ReadOnly) || file.size() < 18 ||
        quint64(file.size()) > maximumInputBytes) {
        return {};
    }
    // ISIZE is an untrusted allocation hint, never proof of the decompressed
    // length. The decoder verifies CRC/length and must consume the whole file.
    if (file.peek(2) != QByteArray::fromHex("1f8b") || !file.seek(file.size() - 4)) {
        return {};
    }
    const QByteArray trailer = file.read(4);
    if (trailer.size() != 4) {
        return {};
    }
    const quint64 bytes = qFromLittleEndian<quint32>(trailer.constData());
    if (bytes == 0 || bytes > std::min(maximumBytes, maximumOutputBytes) || stopped()) {
        return {};
    }
    const uchar* input = file.map(0, file.size());
    if (!input) {
        return {};
    }
    std::unique_ptr<libdeflate_decompressor, decltype(&libdeflate_free_decompressor)> decoder(
        libdeflate_alloc_decompressor(), &libdeflate_free_decompressor);
    if (!decoder) {
        return {};
    }
    QByteArray output(qsizetype(bytes), Qt::Uninitialized);
    size_t consumed = 0, written = 0;
    const auto status =
        libdeflate_gzip_decompress_ex(decoder.get(), input, size_t(file.size()), output.data(),
                                      size_t(output.size()), &consumed, &written);
    if (status != LIBDEFLATE_SUCCESS || consumed != size_t(file.size()) ||
        written != size_t(output.size()) || stopped()) {
        return {};
    }
    return output;
}
} // namespace Licasa
