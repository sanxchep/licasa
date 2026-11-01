#pragma once

#include <QColorSpace>
#include <QImage>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <numeric>
#include <system_error>
#include <thread>
#include <vector>

namespace Licasa {
// Split large reductions at rows that have identical source/target ratios.
// Each band uses Qt's smooth filter; only one-channel rounding at boundaries
// can differ from a single full-frame scale.
inline QImage parallelSmoothScale(const QImage& source, const QSize& target)
{
    const quint64 sourcePixels = quint64(source.width()) * source.height();
    const quint64 targetPixels = quint64(target.width()) * target.height();
    const int groups = std::gcd(source.height(), target.height());
    const int workers = std::min({8, QThread::idealThreadCount(), groups, target.height() / 64});
    const bool packed32 = source.format() == QImage::Format_RGBA8888 ||
                          source.format() == QImage::Format_ARGB32_Premultiplied ||
                          source.format() == QImage::Format_RGB32;
    if (!packed32 || sourcePixels < 16'000'000 || targetPixels > 2'000'000 ||
        source.height() < target.height() * 4 || workers < 4) {
        return source.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    QImage output(target, source.format());
    if (output.isNull()) {
        return {};
    }
    output.setColorSpace(source.colorSpace());
    const uchar* sourceBits = source.constBits();
    uchar* outputBits = output.bits();
    const qsizetype sourceStride = source.bytesPerLine();
    const qsizetype outputStride = output.bytesPerLine();
    const int sourceUnit = source.height() / groups;
    const int targetUnit = target.height() / groups;
    std::atomic_bool failed{false};
    std::vector<std::thread> threads;
    threads.reserve(size_t(workers));
    try {
        for (int worker = 0; worker < workers; ++worker) {
            const int firstGroup = worker * groups / workers;
            const int endGroup = (worker + 1) * groups / workers;
            threads.emplace_back([&, firstGroup, endGroup] {
                const int firstSourceRow = firstGroup * sourceUnit;
                const int sourceRows = (endGroup - firstGroup) * sourceUnit;
                const int firstTargetRow = firstGroup * targetUnit;
                const int targetRows = (endGroup - firstGroup) * targetUnit;
                QImage bandSource(sourceBits + qsizetype(firstSourceRow) * sourceStride,
                                  source.width(), sourceRows, sourceStride, source.format());
                bandSource.setColorSpace(source.colorSpace());
                const QImage band =
                    bandSource.scaled(QSize(target.width(), targetRows), Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation);
                if (band.isNull()) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                for (int row = 0; row < targetRows; ++row) {
                    std::memcpy(outputBits + qsizetype(firstTargetRow + row) * outputStride,
                                band.constScanLine(row), size_t(target.width()) * 4);
                }
            });
        }
    } catch (const std::system_error&) {
        failed.store(true, std::memory_order_relaxed);
    }
    for (auto& thread : threads) {
        thread.join();
    }
    return failed.load(std::memory_order_relaxed)
               ? source.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
               : output;
}

} // namespace Licasa
