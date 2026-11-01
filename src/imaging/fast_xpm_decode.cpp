#include "imaging/fast_xpm_decode.h"
#include "imaging/image_decode_contract.h"

#include <QByteArrayView>
#include <QColor>
#include <QColorSpace>
#include <QFile>
#include <cstring>
#include <vector>

namespace Licasa {
namespace {
class Records {
  public:
    Records(const char* data, qsizetype size) : data_(data), size_(size) {}
    bool begin()
    {
        const QByteArrayView all(data_, std::min<qsizetype>(size_, 4096));
        if (!all.startsWith("/* XPM */")) {
            return false;
        }
        const auto brace = all.indexOf('{');
        if (brace < 9 || all.first(brace).contains('"')) {
            return false;
        }
        pos_ = brace + 1;
        return true;
    }
    bool whitespace()
    {
        const qsizetype start = pos_;
        while (pos_ < size_ && pos_ - start <= 4096) {
            const char c = data_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
                continue;
            }
            if (pos_ + 1 < size_ && c == '/' && data_[pos_ + 1] == '*') {
                pos_ += 2;
                while (pos_ + 1 < size_ && pos_ - start <= 4096 &&
                       !(data_[pos_] == '*' && data_[pos_ + 1] == '/')) {
                    ++pos_;
                }
                if (pos_ + 1 >= size_) {
                    return false;
                }
                pos_ += 2;
                continue;
            }
            return true;
        }
        return pos_ == size_;
    }
    QByteArrayView next(qsizetype maximumLength)
    {
        if (!whitespace() || pos_ >= size_ || data_[pos_++] != '"') {
            return {};
        }
        const qsizetype length = std::min(maximumLength + 1, size_ - pos_);
        const auto* end = static_cast<const char*>(std::memchr(data_ + pos_, '"', size_t(length)));
        if (!end) {
            return {};
        }
        const qsizetype count = end - (data_ + pos_);
        if (std::memchr(data_ + pos_, '\\', size_t(count)) ||
            std::memchr(data_ + pos_, '\n', size_t(count)) ||
            std::memchr(data_ + pos_, '\r', size_t(count))) {
            return {};
        }
        const QByteArrayView result(data_ + pos_, count);
        pos_ += count + 1;
        if (!whitespace()) {
            return {};
        }
        if (pos_ < size_ && data_[pos_] == ',') {
            ++pos_;
        } else if (pos_ >= size_ || data_[pos_] != '}') {
            return {};
        }
        return result;
    }
    bool end()
    {
        if (!whitespace() || pos_ >= size_ || data_[pos_++] != '}') {
            return false;
        }
        if (!whitespace()) {
            return false;
        }
        if (pos_ < size_ && data_[pos_] == ';') {
            ++pos_;
        }
        return whitespace() && pos_ == size_;
    }

  private:
    const char* data_;
    qsizetype size_, pos_ = 0;
};
} // namespace

QImage readFastXpm(const QString& path, quint64 maximumPixels, quint64 maximumBytes,
                   const std::atomic_bool* cancelled)
{
    const auto stopped = [cancelled] {
        return cancelled && cancelled->load(std::memory_order_relaxed);
    };
    QFile file(path);
    if (stopped() || !file.open(QIODevice::ReadOnly) || file.size() < 20 ||
        file.size() > 512LL * 1024 * 1024) {
        return {};
    }
    const uchar* mapped = file.map(0, file.size());
    if (!mapped) {
        return {};
    }
    Records records(reinterpret_cast<const char*>(mapped), file.size());
    if (!records.begin()) {
        return {};
    }
    const QByteArray header = records.next(128).toByteArray().simplified();
    const auto fields = header.split(' ');
    if (fields.size() != 4) {
        return {};
    }
    int values[4];
    for (int i = 0; i < 4; ++i) {
        bool valid = false;
        values[i] = fields[i].toInt(&valid);
        if (!valid || values[i] <= 0) {
            return {};
        }
    }
    const int width = values[0], height = values[1], colors = values[2], cpp = values[3];
    if ((cpp != 1 && cpp != 2) || colors > (cpp == 1 ? 256 : 65536) ||
        !ImageDecodeContract::allows(QSize(width, height), maximumPixels) ||
        quint64(width) * height > maximumBytes / 4) {
        return {};
    }
    const auto code = [cpp](const char* p) {
        return cpp == 1 ? unsigned(uchar(p[0])) : (unsigned(uchar(p[0])) << 8) | uchar(p[1]);
    };
    std::vector<QRgb> palette(cpp == 1 ? 256 : 65536);
    std::vector<uchar> valid(palette.size(), 0);
    bool alpha = false;
    for (int i = 0; i < colors; ++i) {
        const auto row = records.next(128);
        if (row.size() < cpp + 3) {
            return {};
        }
        for (int n = 0; n < cpp; ++n) {
            if (uchar(row[n]) < 0x20 || uchar(row[n]) > 0x7e) {
                return {};
            }
        }
        const unsigned key = code(row.data());
        const auto fields = row.sliced(cpp).toByteArray().simplified().split(' ');
        if (fields.size() != 2 || fields[0] != "c" || valid[key]) {
            return {};
        }
        const QByteArray name = fields[1];
        QRgb color;
        if (name.compare("None", Qt::CaseInsensitive) == 0) {
            color = 0;
            alpha = true;
        } else {
            if (name.size() != 7 || name[0] != '#') {
                return {};
            }
            for (int n = 1; n < 7; ++n) {
                if (!((name[n] >= '0' && name[n] <= '9') || (name[n] >= 'a' && name[n] <= 'f') ||
                      (name[n] >= 'A' && name[n] <= 'F'))) {
                    return {};
                }
            }
            color = QColor(QString::fromLatin1(name)).rgb();
        }
        valid[key] = 1;
        palette[key] = color;
    }
    QImage image(width, height, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    if (image.isNull()) {
        return {};
    }
    for (int y = 0; y < height; ++y) {
        if (stopped()) {
            return {};
        }
        const auto row = records.next(qsizetype(width) * cpp);
        if (row.size() != qsizetype(width) * cpp) {
            return {};
        }
        auto* output = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const unsigned key = code(row.data() + qsizetype(x) * cpp);
            if (!valid[key]) {
                return {};
            }
            output[x] = palette[key];
        }
    }
    if (!records.end() || stopped()) {
        return {};
    }
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}
} // namespace Licasa
