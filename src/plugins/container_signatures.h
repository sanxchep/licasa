#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QtEndian>

#include <algorithm>

namespace Licasa::ContainerSignatures {

inline bool isAvifBrand(const QByteArray& brand) { return brand == "avif" || brand == "avis"; }

inline bool avifSignature(QIODevice* device)
{
    if (!device || !device->isReadable()) {
        return false;
    }

    // Inspect only a bounded prefix of the file-type box.
    const QByteArray header = device->peek(512);
    if (header.size() < 16 || header.mid(4, 4) != "ftyp") {
        return false;
    }

    const quint32 size32 = qFromBigEndian<quint32>(header.constData());
    qsizetype brandOffset = 8;
    quint64 boxSize = size32;
    if (size32 == 1) {
        if (header.size() < 24) {
            return false;
        }
        boxSize = qFromBigEndian<quint64>(header.constData() + 8);
        brandOffset = 16;
    } else if (size32 == 0) {
        const qint64 size = device->size();
        if (size <= 0) {
            return false;
        }
        boxSize = quint64(size);
    }

    if (boxSize < quint64(brandOffset + 8)) {
        return false;
    }
    if (isAvifBrand(header.mid(brandOffset, 4))) {
        return true;
    }

    const qsizetype scanEnd = std::min<quint64>(boxSize, quint64(header.size()));
    for (qsizetype offset = brandOffset + 8; offset + 4 <= scanEnd; offset += 4) {
        if (isAvifBrand(header.mid(offset, 4))) {
            return true;
        }
    }
    return false;
}

inline bool heifSignature(QIODevice* device)
{
    if (!device || !device->isReadable()) {
        return false;
    }
    const QByteArray header = device->peek(256);
    if (header.size() < 16 || header.mid(4, 4) != "ftyp") {
        return false;
    }
    const quint32 length = qFromBigEndian<quint32>(header.constData());
    if (length < 16 || length % 4 != 0) {
        return false;
    }
    // AVIF is handled by its own plugin.
    bool compatible = false;
    for (qsizetype offset = 8; offset + 4 <= std::min<qsizetype>(length, header.size());
         offset += 4) {
        if (offset == 12) {
            continue; // minor version, not a brand
        }
        const auto brand = header.mid(offset, 4);
        if (isAvifBrand(brand)) {
            return false;
        }
        if (brand == "heic" || brand == "heix" || brand == "hevc" || brand == "hevx" ||
            brand == "mif1" || brand == "msf1") {
            compatible = true;
        }
    }
    return compatible;
}

} // namespace Licasa::ContainerSignatures
