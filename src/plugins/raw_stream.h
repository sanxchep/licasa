#pragma once

#include "imaging/image_decode_contract.h"

#include <QIODevice>
#include <QLocale>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <libraw/libraw.h>
#include <limits>

namespace Licasa {
// LibRaw's maintained abstract stream API keeps input in the existing device.
// Like KImageFormats, tolerate a few failed speculative seeks, then stop broken
// streams. All offset/product arithmetic is checked before reaching QIODevice.
class RawStream final : public LibRaw_abstract_datastream {
  public:
    explicit RawStream(QIODevice* device)
        : device_(device), origin_(device ? device->pos() : -1),
          length_(device && origin_ >= 0 && device->size() >= origin_ ? device->size() - origin_
                                                                      : -1)
    {}

    void setReadLimit(quint64 bytes)
    {
        bytesLeft_ = bytes;
        limitReached_ = false;
    }
    bool limitReached() const { return limitReached_; }
    int valid() override
    {
        return device_ && device_->isReadable() && !device_->isSequential() && length_ >= 0;
    }
    int read(void* data, size_t size, size_t count) override
    {
        guard();
        if (!size || !count) {
            return 0;
        }
        if (!data || count > size_t(std::numeric_limits<int>::max()) ||
            size > size_t(std::numeric_limits<qint64>::max()) / count) {
            IOERROR();
        }
        const qint64 total = qint64(size * count);
        const qint64 position = tell();
        if (position < 0 || position > length_) {
            IOERROR();
        }
        const qint64 available = std::min(total, length_ - position);
        if (quint64(available) > bytesLeft_) {
            limitReached_ = true;
            IOERROR();
        }
        qint64 received = 0;
        while (received < available) {
            guard();
            const qint64 amount =
                device_->read(static_cast<char*>(data) + received, available - received);
            if (amount <= 0) {
                fail();
                break;
            }
            received += amount;
            bytesLeft_ -= quint64(amount);
            succeed();
        }
        if (received < total) {
            fail();
        }
        return int(quint64(received) / size);
    }
    int seek(INT64 offset, int whence) override
    {
        guard();
        qint64 base = 0;
        if (whence == SEEK_CUR) {
            base = tell();
        } else if (whence == SEEK_END) {
            base = length_;
        } else if (whence != SEEK_SET) {
            fail();
            return -1;
        }
        if (base < 0 || base > length_ || offset < -base || offset > length_ - base) {
            fail();
            return -1;
        }
        if (!device_->seek(origin_ + base + offset)) {
            fail();
            return -1;
        }
        succeed();
        return 0;
    }
    INT64 tell() override
    {
        if (!device_ || origin_ < 0) {
            return -1;
        }
        const qint64 position = device_->pos();
        return position >= origin_ && position - origin_ <= length_ ? position - origin_ : -1;
    }
    INT64 size() override { return length_; }
    int eof() override { return tell() >= length_; }
    int get_char() override
    {
        unsigned char value = 0;
        return read(&value, 1, 1) == 1 ? value : EOF;
    }
    char* gets(char* output, int capacity) override
    {
        guard();
        if (!output || capacity <= 1) {
            return nullptr;
        }
        int count = 0;
        while (count < capacity - 1) {
            const int value = get_char();
            if (value == EOF) {
                break;
            }
            output[count++] = char(value);
            if (value == '\n') {
                break;
            }
        }
        output[count] = '\0';
        return count ? output : nullptr;
    }
    int scanf_one(const char* format, void* output) override
    {
        guard();
        if (!format || !output) {
            return EOF;
        }
        QByteArray token;
        for (int index = 0; index < 32; ++index) {
            const int value = get_char();
            if (value == EOF) {
                break;
            }
            if (value == ' ' || value == '\t' || value == '\n' || value == '\r') {
                if (token.isEmpty()) {
                    continue;
                }
                break;
            }
            if (!value) {
                break;
            }
            token.append(char(value));
        }
        bool ok = false;
        const QString text = QString::fromLatin1(token);
        if (!std::strcmp(format, "%d")) {
            const int value = QLocale::c().toInt(text, &ok);
            if (ok) {
                *static_cast<int*>(output) = value;
            }
        } else if (!std::strcmp(format, "%f")) {
            const float value = QLocale::c().toFloat(text, &ok);
            ok = ok && std::isfinite(value);
            if (ok) {
                *static_cast<float*>(output) = value;
            }
        }
        return ok ? 1 : EOF;
    }

  private:
    void guard()
    {
        if (ImageDecodeContract::cancelled(device_)) {
            throw LIBRAW_EXCEPTION_CANCELLED_BY_CALLBACK;
        }
        if (!valid() || failures_ > 12) {
            IOERROR();
        }
    }
    void fail() { failures_ += 3; }
    void succeed() { failures_ = std::max(0, failures_ - 1); }
    QIODevice* device_;
    qint64 origin_;
    qint64 length_;
    quint64 bytesLeft_ = 8 * 1024 * 1024;
    int failures_ = 0;
    bool limitReached_ = false;
};
} // namespace Licasa
