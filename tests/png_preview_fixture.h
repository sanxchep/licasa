#pragma once

#include <QFile>

#include <png.h>

#include <cstdio>
#include <vector>

namespace LicasaTest {
inline bool writePlainLargePng(const QString& path, bool alpha, int extent = 5000)
{
    const QByteArray encoded = QFile::encodeName(path);
    FILE* file = std::fopen(encoded.constData(), "wb");
    if (!file) {
        return false;
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!info) {
        if (png) {
            png_destroy_write_struct(&png, nullptr);
        }
        std::fclose(file);
        return false;
    }
    std::vector<uchar> row(size_t(extent) * (alpha ? 4 : 3));
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(file);
        return false;
    }
    png_init_io(png, file);
    png_set_compression_level(png, 1);
    png_set_IHDR(png, info, extent, extent, 8, alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png, info);
    const int channels = alpha ? 4 : 3;
    for (int x = 0; x < extent; ++x) {
        row[size_t(x) * channels] = 30;
        row[size_t(x) * channels + 1] = 80;
        row[size_t(x) * channels + 2] = 140;
        if (alpha) {
            row[size_t(x) * channels + 3] = 255;
        }
    }
    for (int y = 0; y < extent; ++y) {
        if (y == 2500) {
            row[size_t(2500) * channels] = 210;
            row[size_t(2500) * channels + 1] = 40;
            row[size_t(2500) * channels + 2] = 90;
            if (alpha) {
                row[size_t(2500) * channels + 3] = 120;
            }
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return std::fclose(file) == 0;
}
} // namespace LicasaTest
