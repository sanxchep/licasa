#include "imaging/progressive_jpeg_preview.h"

#include <QColorSpace>
#include <QFile>
#include <QImageReader>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <jpeglib.h>
#include <setjmp.h>

namespace Licasa {
namespace {
constexpr int earlyScan = 4;
constexpr quint64 maximumFastPathPixels = 120ull * 1000 * 1000;
constexpr quint64 maximumPreviewBytes = 64ull * 1024 * 1024;

struct JpegError {
    jpeg_error_mgr base;
    jmp_buf jump;
};

struct State {
    jpeg_decompress_struct jpeg{};
    JpegError error{};
    FILE* file = nullptr;
    unsigned char* pixels = nullptr;
    bool created = false;
    unsigned width = 0;
    unsigned height = 0;
    unsigned stride = 0;
};

void onJpegError(j_common_ptr info)
{
    auto* error = reinterpret_cast<JpegError*>(info->err);
    longjmp(error->jump, 1);
}

void onJpegMessage(j_common_ptr info, int level)
{
    if (level < 0) {
        onJpegError(info);
    }
}

void release(State* state)
{
    if (state->created) {
        jpeg_destroy_decompress(&state->jpeg);
    }
    if (state->file) {
        std::fclose(state->file);
    }
    std::free(state->pixels);
    delete state;
}

bool hasEmbeddedIcc(const jpeg_decompress_struct& jpeg)
{
    constexpr char prefix[] = "ICC_PROFILE";
    for (jpeg_saved_marker_ptr marker = jpeg.marker_list; marker; marker = marker->next) {
        if (marker->marker == JPEG_APP0 + 2 && marker->data_length >= sizeof(prefix) &&
            std::memcmp(marker->data, prefix, sizeof(prefix)) == 0) {
            return true;
        }
    }
    return false;
}

int scaleDenominator(const jpeg_decompress_struct& jpeg, const QSize& requestedSize)
{
    for (const int denominator : {8, 4, 2}) {
        if ((jpeg.image_width + denominator - 1) / denominator >= unsigned(requestedSize.width()) &&
            (jpeg.image_height + denominator - 1) / denominator >=
                unsigned(requestedSize.height())) {
            return denominator;
        }
    }
    return 1;
}

State* decodeFirstScans(const QByteArray& fileName, const QSize& requestedSize,
                        quint64 maximumPixels, const std::atomic_bool* cancelled)
{
    auto* state = new State;
    state->file = std::fopen(fileName.constData(), "rb");
    if (!state->file) {
        release(state);
        return nullptr;
    }
    state->jpeg.err = jpeg_std_error(&state->error.base);
    state->error.base.error_exit = onJpegError;
    state->error.base.emit_message = onJpegMessage;
    if (setjmp(state->error.jump)) {
        release(state);
        return nullptr;
    }
    jpeg_create_decompress(&state->jpeg);
    state->created = true;
    jpeg_stdio_src(&state->jpeg, state->file);
    jpeg_save_markers(&state->jpeg, JPEG_APP0 + 2, 12);
    jpeg_read_header(&state->jpeg, TRUE);
    const quint64 sourcePixels = quint64(state->jpeg.image_width) * state->jpeg.image_height;
    if (!state->jpeg.progressive_mode || sourcePixels == 0 || sourcePixels > maximumPixels ||
        sourcePixels > maximumFastPathPixels ||
        (state->jpeg.jpeg_color_space != JCS_YCbCr &&
         state->jpeg.jpeg_color_space != JCS_GRAYSCALE) ||
        hasEmbeddedIcc(state->jpeg)) {
        release(state);
        return nullptr;
    }
    state->jpeg.scale_num = 1;
    state->jpeg.scale_denom = scaleDenominator(state->jpeg, requestedSize);
    state->jpeg.out_color_space = JCS_RGB;
    state->jpeg.buffered_image = TRUE;
    jpeg_calc_output_dimensions(&state->jpeg);
    const quint64 outputBytes = quint64(state->jpeg.output_width) * state->jpeg.output_height * 3;
    const int allocationMiB = QImageReader::allocationLimit();
    if (outputBytes == 0 || outputBytes > maximumPreviewBytes ||
        (allocationMiB > 0 && outputBytes > quint64(allocationMiB) * 1024 * 1024)) {
        release(state);
        return nullptr;
    }
    jpeg_start_decompress(&state->jpeg);
    jpeg_start_output(&state->jpeg, earlyScan);
    state->width = state->jpeg.output_width;
    state->height = state->jpeg.output_height;
    state->stride = state->width * 3;
    state->pixels = static_cast<unsigned char*>(std::malloc(outputBytes));
    if (!state->pixels) {
        release(state);
        return nullptr;
    }
    while (state->jpeg.output_scanline < state->jpeg.output_height) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            release(state);
            return nullptr;
        }
        JSAMPROW row = state->pixels + size_t(state->jpeg.output_scanline) * state->stride;
        if (jpeg_read_scanlines(&state->jpeg, &row, 1) != 1) {
            release(state);
            return nullptr;
        }
    }
    // jpeg_destroy_decompress() discards the remaining scans; the ordinary Qt
    // reader still supplies the final full-detail image in a separate request.
    return state;
}
} // namespace

QImage readEarlyProgressiveJpegPreview(const QString& path, const QSize& requestedSize,
                                       quint64 maximumPixels, const std::atomic_bool* cancelled)
{
    if (!requestedSize.isValid()) {
        return {};
    }
    State* state =
        decodeFirstScans(QFile::encodeName(path), requestedSize, maximumPixels, cancelled);
    if (!state) {
        return {};
    }
    QImage image(state->pixels, int(state->width), int(state->height), int(state->stride),
                 QImage::Format_RGB888);
    if (image.isNull()) {
        release(state);
        return {};
    }
    // The output image must own its pixels before the libjpeg state is freed.
    QImage owned = image.copy();
    release(state);
    if (owned.isNull()) {
        return {};
    }
    owned.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return owned.size() == requestedSize
               ? owned
               : owned.scaled(requestedSize, Qt::IgnoreAspectRatio, Qt::FastTransformation);
}

} // namespace Licasa
