#include "imaging/image_decode_contract.h"
#include "raw_stream.h"

#include <QBuffer>
#include <QColorSpace>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageIOHandler>
#include <QThread>
#include <QTransform>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>

namespace Contract = Licasa::ImageDecodeContract;
namespace {
constexpr quint64 metadataReadLimit = 8 * 1024 * 1024;

quint64 pixels(const QSize& size)
{
    return size.isValid() ? quint64(size.width()) * quint64(size.height()) : 0;
}
bool traceRawDevelopment() { return qEnvironmentVariableIsSet("LICASA_TRACE_RAW_DEVELOP"); }
QImage orient(QImage image, int flip)
{
    switch (flip) {
    case 1:
        return image.mirrored(true, false);
    case 2:
        return image.mirrored(false, true);
    case 3:
        return image.transformed(QTransform().rotate(180));
    case 4:
        return image.transformed(QTransform().rotate(90)).mirrored(true, false);
    case 5:
        return image.transformed(QTransform().rotate(270));
    case 6:
        return image.transformed(QTransform().rotate(90));
    case 7:
        return image.transformed(QTransform().rotate(90)).mirrored(false, true);
    default:
        return image;
    }
}

class RawHandler final : public QImageIOHandler {
    struct MappedInput {
        QFile* file = nullptr;
        uchar* data = nullptr;
        size_t size = 0;

        ~MappedInput()
        {
            if (file && data) {
                file->unmap(data);
            }
        }

        bool map(QIODevice* device, qint64 origin, qint64 length, quint64 byteAllowance)
        {
            if (qEnvironmentVariableIsSet("LICASA_DISABLE_RAW_MMAP") || origin < 0 || length <= 0 ||
                quint64(length) > byteAllowance ||
                quint64(length) > quint64(std::numeric_limits<size_t>::max())) {
                return false;
            }
            auto* localFile = qobject_cast<QFile*>(device);
            if (!localFile) {
                return false;
            }
            uchar* mapping = localFile->map(origin, length);
            if (!mapping) {
                return false;
            }
            file = localFile;
            data = mapping;
            size = size_t(length);
            return true;
        }
    };

    struct Native {
        // LibRaw retains either the mapped buffer or this stream. Member order is
        // intentional: LibRaw is destroyed before the stream and mapping.
        MappedInput mappedInput;
        Licasa::RawStream stream;
        LibRaw raw;
        bool usesMappedInput = false;
        explicit Native(QIODevice* device) : stream(device) {}
    };

  public:
    bool canRead() const override { return const_cast<RawHandler*>(this)->readMetadata(); }
    bool supportsOption(ImageOption option) const override
    {
        return option == Size || option == ScaledSize || option == ImageFormat ||
               option == Description;
    }
    QVariant option(ImageOption option) const override
    {
        if (option == ScaledSize) {
            return scaledSize_;
        }
        if (option == ImageFormat) {
            return QImage::Format_RGB888;
        }
        if (!const_cast<RawHandler*>(this)->readMetadata()) {
            return {};
        }
        if (option == Size) {
            return size_;
        }
        if (option == Description) {
            return QStringLiteral("Backend: LibRaw %1\n\nPreviewPath: %2")
                .arg(QString::fromLatin1(LibRaw::version()), previewPath_);
        }
        return {};
    }
    void setOption(ImageOption option, const QVariant& value) override
    {
        if (option == ScaledSize) {
            scaledSize_ = value.toSize();
        }
    }
    int imageCount() const override { return canRead() ? 1 : 0; }
    bool read(QImage* output) override
    {
        if (!output || finished_ || !readMetadata() || Contract::cancelled(device())) {
            return false;
        }
        finished_ = true;
        QImage image;
        try {
            if (device()->property(Contract::fullDetailProperty).toBool()) {
                image = develop();
            } else {
                image = preview();
            }
        } catch (const std::bad_alloc&) {
            fail(QStringLiteral("There is not enough memory to read this RAW image."));
        } catch (...) {
            fail(QStringLiteral("The RAW image is damaged or reading was cancelled."));
        }
        native_.reset();
        if (image.isNull() || Contract::cancelled(device())) {
            return false;
        }
        const bool interactiveDevelopment =
            device()->property(Contract::rawInteractiveDevelopmentProperty).toBool();
        if (interactiveDevelopment && image.format() == QImage::Format_RGB888 &&
            !qEnvironmentVariableIsSet("LICASA_RAW_RGB888_BASE_LEGACY")) {
            QElapsedTimer convertTimer;
            convertTimer.start();
            QImage converted = image.convertToFormat(QImage::Format_ARGB32);
            if (converted.isNull()) {
                return fail(QStringLiteral(
                    "There is not enough memory to prepare the interactive RAW raster."));
            }
            image = std::move(converted);
            if (traceRawDevelopment()) {
                qInfo().noquote()
                    << QStringLiteral("[Licasa] RAW interactive base: format=argb32 convert=%1 ms "
                                      "bytes=%2 MiB")
                           .arg(double(convertTimer.nsecsElapsed()) / 1000000.0, 0, 'f', 2)
                           .arg(double(image.sizeInBytes()) / (1024.0 * 1024.0), 0, 'f', 1);
            }
        }
        if (!Contract::allows(image.size(), maximumPixels_)) {
            return limitFailure();
        }
        if (scaledSize_.isValid() && !scaledSize_.isEmpty() &&
            (image.width() > scaledSize_.width() || image.height() > scaledSize_.height())) {
            const QSize target = image.size().scaled(scaledSize_, Qt::KeepAspectRatio);
            if (!Contract::allows(target, maximumPixels_)) {
                return limitFailure();
            }
            image = image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        if (image.isNull() || Contract::cancelled(device())) {
            return false;
        }
        // LibRaw's SDR development is sRGB. Embedded JPEG/JXL retains its own
        // profile when present; plain camera RGB previews have an sRGB fallback.
        if (!image.colorSpace().isValid()) {
            image.setColorSpace(QColorSpace::SRgb);
        }
        device()->setProperty(Contract::errorProperty, {});
        *output = std::move(image);
        return true;
    }

  private:
    bool fail(const QString& error)
    {
        if (device()) {
            device()->setProperty(Contract::errorProperty, error);
        }
        return false;
    }
    bool limitFailure()
    {
        return fail(QStringLiteral("The RAW image exceeds the selected image limit."));
    }
    bool nativeFailure(int code)
    {
        if (Contract::cancelled(device())) {
            return fail(QStringLiteral("RAW reading was cancelled."));
        }
        if (native_ && native_->stream.limitReached()) {
            return fail(
                QStringLiteral("The RAW image exceeds the bounded metadata or input allowance."));
        }
        return fail(QStringLiteral("The RAW image could not be read: %1.")
                        .arg(QString::fromLatin1(libraw_strerror(code))));
    }
    bool openNative(bool preferMappedInput = false)
    {
        if (native_ && (!preferMappedInput || native_->usesMappedInput)) {
            return true;
        }
        if (!device() || !device()->isReadable() || device()->isSequential() ||
            Contract::cancelled(device()) || !device()->seek(origin_)) {
            return false;
        }
        // Metadata/thumbnail parsing keeps the bounded QIODevice stream. Full
        // local-file development may reopen through LibRaw's native buffer
        // datastream backed by QFile::map, eliminating virtual QIODevice
        // read/seek transitions in lossless RAW unpackers.
        native_ = std::make_unique<Native>(device());
        auto& raw = native_->raw;
        raw.imgdata.rawparams.max_raw_memory_mb =
            unsigned(std::min<quint64>((workingBytes_ + 1024 * 1024 - 1) / (1024 * 1024),
                                       std::numeric_limits<unsigned>::max()));
        raw.imgdata.rawparams.options |= LIBRAW_RAWOPTIONS_ALLOW_JPEGXL_PREVIEWS;
        // Prefer the camera-recorded white balance and embedded matrix. The
        // camera JPEG preview can still have a proprietary vendor tone/look,
        // but the developed raster must at least use a transfer curve that
        // matches the sRGB QColorSpace tag we attach below.
        raw.imgdata.params.use_camera_wb = 1;
        raw.imgdata.params.use_auto_wb = 0;
        // Keep export/save quality on AHD, but use OpenMP-capable PPG
        // for the viewer's full-resolution interactive raster. The latter is
        // replaced by export-quality development when a save/export explicitly
        // reads the sensor data; it is not an output-quality policy change.
        const bool interactiveDevelopment =
            device()->property(Contract::rawInteractiveDevelopmentProperty).toBool();
        raw.imgdata.params.user_qual = interactiveDevelopment ? 2 : 3;
        // A no-preview RAW gets one bounded, half-size development before the
        // final raster so the viewer can show useful pixels instead of a grey
        // loading card. This uses the same white-balance/output-color pipeline
        // as final development; only spatial detail is reduced.
        raw.imgdata.params.half_size =
            device()->property(Contract::rawFastDevelopmentProperty).toBool() ? 1 : 0;
        raw.imgdata.params.output_color = 1;
        raw.imgdata.params.output_bps = 8;
        raw.imgdata.params.gamm[0] = 1.0 / 2.4;
        raw.imgdata.params.gamm[1] = 12.92;
        raw.set_progress_handler(
            [](void* context, LibRaw_progress, int, int) {
                return Contract::cancelled(static_cast<QIODevice*>(context)) ? 1 : 0;
            },
            device());
        int result = LIBRAW_UNSPECIFIED_ERROR;
        if (preferMappedInput) {
            const quint64 inputAllowance =
                workingBytes_ > std::numeric_limits<quint64>::max() - metadataReadLimit
                    ? std::numeric_limits<quint64>::max()
                    : workingBytes_ + metadataReadLimit;
            if (native_->mappedInput.map(device(), origin_, native_->stream.size(),
                                         inputAllowance)) {
                result = raw.open_buffer(native_->mappedInput.data, native_->mappedInput.size);
                if (result != LIBRAW_SUCCESS) {
                    return nativeFailure(result);
                }
                native_->usesMappedInput = true;
                if (traceRawDevelopment()) {
                    qInfo().noquote() << QStringLiteral("[Licasa] RAW input: mmap-buffer");
                }
                return true;
            }
            if (traceRawDevelopment()) {
                qInfo().noquote() << QStringLiteral("[Licasa] RAW input: qiodevice-stream");
            }
        }
        result = raw.open_datastream(&native_->stream);
        if (result != LIBRAW_SUCCESS) {
            return nativeFailure(result);
        }
        return true;
    }
    bool readMetadata()
    {
        if (metadataRead_) {
            return size_.isValid();
        }
        metadataRead_ = true;
        if (!device() || device()->isSequential() || Contract::cancelled(device())) {
            return false;
        }
        origin_ = device()->pos();
        maximumPixels_ = Contract::pixelBudget(device());
        // One ImageResourcePolicy snapshot governs raster admission and this
        // native working allowance. This is not a second megapixel preference.
        workingBytes_ =
            std::min<quint64>(maximumPixels_,
                              (quint64(std::numeric_limits<qint64>::max()) - metadataReadLimit) /
                                  64) *
            64;
        try {
            if (!openNative()) {
                native_.reset();
                return false;
            }
            auto& raw = native_->raw;
            int w = 0, h = 0, colors = 0, bits = 0;
            raw.get_mem_image_format(&w, &h, &colors, &bits);
            const auto& s = raw.imgdata.sizes;
            sensorSize_ = QSize(s.raw_width, s.raw_height);
            if (w <= 0 || h <= 0 || !sensorSize_.isValid() || !raw.imgdata.idata.raw_count) {
                native_.reset();
                return fail(
                    QStringLiteral("The RAW decoder cannot report valid image dimensions."));
            }
            size_ = QSize(w, h); // LibRaw reports and produces oriented pixels.
            device()->setProperty(Contract::previewFirstProperty, true);
            device()->setProperty(Contract::sensorSizeProperty, sensorSize_);
            const auto& list = raw.imgdata.thumbs_list;
            count_ = std::clamp(list.thumbcount, 0, LIBRAW_THUMBNAIL_MAXCOUNT);
            for (int i = 0; i < count_; ++i) {
                order_[i] = i;
            }
            std::stable_sort(order_.begin(), order_.begin() + count_, [&list](int a, int b) {
                const auto& x = list.thumblist[a];
                const auto& y = list.thumblist[b];
                return quint64(x.twidth) * x.theight > quint64(y.twidth) * y.theight;
            });

            // The multiple-thumbnail list may contain an entry whose dimensions
            // are unknown at metadata time, and some Canon CR2 files expose the
            // useful full-size JPEG only through LibRaw's legacy/default
            // thumbnail fields. Keep the list for explicit selection, but also
            // compare it with LibRaw's documented default (largest) preview.
            QSize bestListedPreview;
            for (int i = 0; i < count_; ++i) {
                const auto& thumb = list.thumblist[order_[i]];
                if (!admissible(thumb)) {
                    continue;
                }
                const QSize candidate(thumb.twidth, thumb.theight);
                if (!candidate.isValid() || candidate.isEmpty()) {
                    continue;
                }
                bestListedPreview = candidate;
                break;
            }

            const auto& defaultThumb = raw.imgdata.thumbnail;
            const QSize defaultPreview(defaultThumb.twidth, defaultThumb.theight);
            const qint64 thumbLimit = qint64(
                std::min<quint64>(workingBytes_, quint64(std::numeric_limits<qint64>::max())));
            const bool defaultPreviewUsable =
                raw.thumbOK(thumbLimit) != 0 && defaultThumb.tlength >= 64 &&
                defaultThumb.tlength <= workingBytes_ && defaultPreview.isValid() &&
                !defaultPreview.isEmpty() && Contract::allows(defaultPreview, maximumPixels_);

            defaultPreviewPreferred_ =
                defaultPreviewUsable && pixels(defaultPreview) > pixels(bestListedPreview);
            QSize advertisedPreview = defaultPreviewPreferred_ ? defaultPreview : bestListedPreview;
            if (advertisedPreview.isValid()) {
                if (s.flip & 4) {
                    advertisedPreview.transpose();
                }
                device()->setProperty(Contract::embeddedPreviewSizeProperty, advertisedPreview);
            }
            return true;
        } catch (...) {
            native_.reset();
            return fail(QStringLiteral("The RAW metadata is damaged or reading was cancelled."));
        }
    }
    bool admissible(const libraw_thumbnail_item_t& thumb) const
    {
        if (!native_ || thumb.toffset <= 0 || thumb.toffset > native_->stream.size() ||
            quint64(thumb.tlength) > quint64(native_->stream.size() - thumb.toffset) ||
            thumb.tlength < 64 || thumb.tlength > workingBytes_) {
            return false;
        }
        const bool compressed = thumb.tformat == LIBRAW_INTERNAL_THUMBNAIL_JPEG ||
                                thumb.tformat == LIBRAW_INTERNAL_THUMBNAIL_JPEGXL;
        const QSize size(thumb.twidth, thumb.theight);
        // Compressed previews do not get the development pipeline's much
        // larger scratch allowance. Bound retained input by admitted pixels
        // plus the fixed metadata allowance before LibRaw copies it.
        const quint64 previewPixels =
            pixels(size) ? std::min(pixels(size), maximumPixels_) : maximumPixels_;
        const quint64 inputAllowance =
            std::min<quint64>(previewPixels,
                              (quint64(std::numeric_limits<qint64>::max()) - metadataReadLimit) /
                                  4) *
                4 +
            metadataReadLimit;
        if (compressed && thumb.tlength > inputAllowance) {
            return false;
        }
        // A JXL thumbnail can omit TIFF dimensions. Its nested, explicitly
        // selected Qt reader must admit its declared raster before decoding.
        if (!(compressed && !pixels(size)) && !Contract::allows(size, maximumPixels_)) {
            return false;
        }
        if (!compressed) {
            const int colors = (thumb.tmisc >> 5) & 7;
            const int bytesPerSample = (thumb.tmisc & 31) / 8;
            if ((colors != 1 && colors != 3) || (bytesPerSample != 1 && bytesPerSample != 2)) {
                return false;
            }
            if (pixels(size) * quint64(colors) * quint64(bytesPerSample) > thumb.tlength) {
                return false;
            }
        }
        return compressed || thumb.tformat == LIBRAW_INTERNAL_THUMBNAIL_PPM ||
               thumb.tformat == LIBRAW_INTERNAL_THUMBNAIL_PPM16;
    }
    QImage decodeUnpackedThumbnail(int fallbackFlip)
    {
        if (!native_) {
            return {};
        }
        QImage image;
        const auto& data = native_->raw.imgdata.thumbnail;
        if (data.tformat == LIBRAW_THUMBNAIL_BITMAP) {
            const QSize dimensions(data.twidth, data.theight);
            const int colors = data.tcolors;
            if (!Contract::allows(dimensions, maximumPixels_) || !data.thumb ||
                (colors != 1 && colors != 3) ||
                pixels(dimensions) * quint64(colors) > data.tlength ||
                data.tlength > workingBytes_) {
                return {};
            }
            if (!allocateImage(dimensions,
                               colors == 3 ? QImage::Format_RGB888 : QImage::Format_Grayscale8,
                               &image)) {
                return {};
            }
            const qsizetype rowBytes = qsizetype(dimensions.width()) * colors;
            for (int y = 0; y < dimensions.height(); ++y) {
                if (Contract::cancelled(device())) {
                    return {};
                }
                std::memcpy(image.scanLine(y), data.thumb + qsizetype(y) * rowBytes,
                            size_t(rowBytes));
            }
            image = orient(std::move(image), fallbackFlip);
            device()->setProperty("_licasaNativeRasterPixels", pixels(dimensions));
        } else if (data.tformat == LIBRAW_THUMBNAIL_JPEG ||
                   data.tformat == LIBRAW_THUMBNAIL_JPEGXL) {
            // LibRaw adds a valid EXIF orientation to JPEG previews which do
            // not carry one. It retains existing preview EXIF and ICC.
            using Memory =
                std::unique_ptr<libraw_processed_image_t, decltype(&LibRaw::dcraw_clear_mem)>;
            Memory processed(native_->raw.dcraw_make_mem_thumb(), LibRaw::dcraw_clear_mem);
            if (!processed || processed->data_size > workingBytes_) {
                return {};
            }
            auto bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(processed->data),
                                                 processed->data_size);
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer, data.tformat == LIBRAW_THUMBNAIL_JPEG ? "jpeg" : "jxl");
            reader.setAutoDetectImageFormat(false);
            reader.setAutoTransform(true);
            Contract::configure(reader, maximumPixels_);
            buffer.setProperty(Contract::cancellationProperty,
                               device()->property(Contract::cancellationProperty));
            const QSize declared = reader.size();
            if (!Contract::allows(declared, maximumPixels_)) {
                return {};
            }
            QSize target = scaledSize_;
            const auto transformation = reader.transformation();
            if (transformation.testFlag(TransformationRotate90)) {
                target.transpose();
            }
            if (target.isValid() &&
                (declared.width() > target.width() || declared.height() > target.height())) {
                reader.setScaledSize(declared.scaled(target, Qt::KeepAspectRatio));
            }
            image = reader.read();
            device()->setProperty("_licasaNativeRasterPixels", pixels(declared));
            if (data.tformat == LIBRAW_THUMBNAIL_JPEGXL) {
                image = orient(std::move(image), fallbackFlip);
            }
        }
        return image;
    }

    QImage defaultLargestPreview()
    {
        native_.reset();
        if (!openNative() || Contract::cancelled(device())) {
            return {};
        }
        auto& raw = native_->raw;
        const qint64 thumbLimit =
            qint64(std::min<quint64>(workingBytes_, quint64(std::numeric_limits<qint64>::max())));
        if (!raw.thumbOK(thumbLimit)) {
            return {};
        }
        native_->stream.setReadLimit(workingBytes_);
        const int code = raw.unpack_thumb();
        if (code != LIBRAW_SUCCESS) {
            nativeFailure(code);
            return {};
        }
        QImage image = decodeUnpackedThumbnail(raw.imgdata.sizes.flip);
        if (!image.isNull() && Contract::allows(image.size(), maximumPixels_)) {
            previewPath_ =
                QStringLiteral("embedded RAW preview (LibRaw largest/default); no demosaic");
            device()->setProperty("_licasaRawDemosaic", false);
            if (!device()->property(Contract::embeddedPreviewSizeProperty).toSize().isValid()) {
                device()->setProperty(Contract::embeddedPreviewSizeProperty, image.size());
            }
            return image;
        }
        return {};
    }

    QImage preview()
    {
        // If metadata says LibRaw's legacy/default thumbnail is larger than any
        // explicit list entry, use it first. This is important for Canon CR2:
        // Exif IFD0 may contain a full-resolution PreviewImage even when the
        // multi-thumbnail representation is incomplete or not directly
        // admissible by our explicit-list path.
        if (defaultPreviewPreferred_) {
            QImage image = defaultLargestPreview();
            if (!image.isNull()) {
                return image;
            }
        }

        for (int i = 0; i < count_ && !Contract::cancelled(device()); ++i) {
            if (!openNative()) {
                break;
            }
            const int index = order_[i];
            if (index >= native_->raw.imgdata.thumbs_list.thumbcount) {
                continue;
            }
            const auto thumb = native_->raw.imgdata.thumbs_list.thumblist[index];
            if (!admissible(thumb)) {
                continue;
            }
            native_->stream.setReadLimit(workingBytes_);
            const int code = native_->raw.unpack_thumb_ex(index);
            if (code != LIBRAW_SUCCESS) {
                nativeFailure(code);
                if (LIBRAW_FATAL_ERROR(code)) {
                    native_.reset();
                }
                continue;
            }
            const int flip = thumb.tflip <= 7 ? thumb.tflip : native_->raw.imgdata.sizes.flip;
            QImage image = decodeUnpackedThumbnail(flip);
            if (!image.isNull() && Contract::allows(image.size(), maximumPixels_)) {
                previewPath_ = QStringLiteral("embedded RAW preview; no demosaic");
                device()->setProperty("_licasaRawDemosaic", false);
                return image;
            }
        }

        // LibRaw explicitly documents unpack_thumb() as the default/largest
        // preview selector. Keep it as a final compatibility fallback rather
        // than dropping immediately into RAW unpack/demosaic when an otherwise
        // usable camera JPEG was not represented by the explicit list path.
        if (!defaultPreviewPreferred_ && !Contract::cancelled(device())) {
            QImage image = defaultLargestPreview();
            if (!image.isNull()) {
                return image;
            }
        }

        fail(QStringLiteral(
            "No usable embedded preview is available; full RAW development is required."));
        return {};
    }
    QImage develop()
    {
        if (!Contract::allows(sensorSize_, maximumPixels_) ||
            !Contract::allows(size_, maximumPixels_)) {
            limitFailure();
            return {};
        }
        if (!openNative(true)) {
            return {};
        }
        // A compressed file cannot force unbounded repeated reads. Metadata and
        // raster input are bounded independently; no whole-file copy is made.
        native_->stream.setReadLimit(workingBytes_ + metadataReadLimit);
        device()->setProperty("_licasaRawDemosaic", true);
        device()->setProperty("_licasaNativeRasterPixels", pixels(sensorSize_));
        const bool interactiveDevelopment =
            device()->property(Contract::rawInteractiveDevelopmentProperty).toBool();
        // Qualify only the two decoder families measured with exact sensor
        // pixels. LibRaw itself returns to its native unpacker if RawSpeed3
        // declines or fails, and export continues to use native unpacking.
        constexpr unsigned rawspeedCapabilities = LIBRAW_CAPS_RAWSPEED3 | LIBRAW_CAPS_RAWSPEED_BITS;
        const auto* localFile = qobject_cast<QFile*>(device());
        const QString suffix =
            localFile ? QFileInfo(localFile->fileName()).suffix().toLower() : QString();
        const char* decoder = native_->raw.unpack_function_name();
        const bool qualifiedDecoder = (suffix == QStringLiteral("dng") && decoder &&
                                       std::strcmp(decoder, "lossless_dng_load_raw()") == 0) ||
                                      (suffix == QStringLiteral("cr2") && decoder &&
                                       std::strcmp(decoder, "lossless_jpeg_load_raw()") == 0);
        const bool tryRawspeed3 =
            interactiveDevelopment && native_->usesMappedInput && qualifiedDecoder &&
            native_->mappedInput.size <= 256 * 1024 * 1024 &&
            !qEnvironmentVariableIsSet("LICASA_DISABLE_RAWSPEED3") &&
            (LibRaw::capabilities() & rawspeedCapabilities) == rawspeedCapabilities;
        if (tryRawspeed3) {
            native_->raw.imgdata.rawparams.use_rawspeed = LIBRAW_RAWSPEEDV3_USE;
        }
        QElapsedTimer totalTimer;
        QElapsedTimer stageTimer;
        totalTimer.start();
        stageTimer.start();
        int code = native_->raw.unpack();
        const qint64 unpackNs = stageTimer.nsecsElapsed();
        if (traceRawDevelopment() && tryRawspeed3) {
            const unsigned warnings = native_->raw.imgdata.process_warnings;
            qInfo().noquote() << QStringLiteral("[Licasa] RAW unpack: rawspeed3_processed=%1 "
                                                "problem=%2 unsupported=%3 notlisted=%4")
                                     .arg(bool(warnings & LIBRAW_WARN_RAWSPEED3_PROCESSED))
                                     .arg(bool(warnings & LIBRAW_WARN_RAWSPEED3_PROBLEM))
                                     .arg(bool(warnings & LIBRAW_WARN_RAWSPEED3_UNSUPPORTED))
                                     .arg(bool(warnings & LIBRAW_WARN_RAWSPEED3_NOTLISTED));
        }
        if (code != LIBRAW_SUCCESS) {
            nativeFailure(code);
            return {};
        }
        if (Contract::cancelled(device())) {
            return {};
        }
        stageTimer.restart();
        code = native_->raw.dcraw_process();
        const qint64 processNs = stageTimer.nsecsElapsed();
        if (code != LIBRAW_SUCCESS) {
            nativeFailure(code);
            return {};
        }
        if (Contract::cancelled(device())) {
            return {};
        }
        int w = 0, h = 0, colors = 0, bits = 0;
        native_->raw.get_mem_image_format(&w, &h, &colors, &bits);
        const QSize dimensions(w, h);
        if (!Contract::allows(dimensions, maximumPixels_) || bits != 8 ||
            (colors != 1 && colors != 3)) {
            limitFailure();
            return {};
        }
        // Keep the short, parallel output-copy burst off the preview's first
        // display frame. Unpack and demosaic have already run alongside it.
        const auto previewFlagValue =
            device()->property(Contract::previewPresentedProperty).value<quintptr>();
        const auto* previewPresented = reinterpret_cast<const std::atomic_bool*>(previewFlagValue);
        if (interactiveDevelopment && previewPresented) {
            QElapsedTimer previewWait;
            previewWait.start();
            while (!previewPresented->load(std::memory_order_acquire) &&
                   !Contract::cancelled(device()) && previewWait.elapsed() < 200) {
                QThread::msleep(1);
            }
            if (traceRawDevelopment()) {
                qInfo().noquote() << QStringLiteral("[Licasa] RAW copy preview wait=%1 ms")
                                         .arg(previewWait.elapsed());
            }
        }
        QImage image;
        if (!allocateImage(dimensions,
                           colors == 3 ? QImage::Format_RGB888 : QImage::Format_Grayscale8,
                           &image)) {
            return {};
        }
        if (image.bytesPerLine() > std::numeric_limits<int>::max()) {
            return {};
        }
        stageTimer.restart();
        code = native_->raw.copy_mem_image(image.bits(), int(image.bytesPerLine()), 0);
        const qint64 copyNs = stageTimer.nsecsElapsed();
        if (code != LIBRAW_SUCCESS) {
            nativeFailure(code);
            return {};
        }
        if (traceRawDevelopment()) {
            qInfo().noquote()
                << QStringLiteral(
                       "[Licasa] RAW develop: mode=%1 size=%2x%3 unpack=%4 ms process=%5 ms "
                       "copy=%6 ms total=%7 ms OMP_NUM_THREADS=%8")
                       .arg(interactiveDevelopment ? QStringLiteral("interactive-ppg")
                                                   : QStringLiteral("export-ahd"))
                       .arg(dimensions.width())
                       .arg(dimensions.height())
                       .arg(double(unpackNs) / 1000000.0, 0, 'f', 2)
                       .arg(double(processNs) / 1000000.0, 0, 'f', 2)
                       .arg(double(copyNs) / 1000000.0, 0, 'f', 2)
                       .arg(double(totalTimer.nsecsElapsed()) / 1000000.0, 0, 'f', 2)
                       .arg(qEnvironmentVariable("OMP_NUM_THREADS"));
        }
        previewPath_ = device()->property(Contract::rawFastDevelopmentProperty).toBool()
                           ? QStringLiteral("fast half-size RAW development")
                       : interactiveDevelopment
                           ? QStringLiteral("interactive full RAW development (PPG)")
                           : QStringLiteral("full RAW development (AHD)");
        image.setColorSpace(QColorSpace::SRgb);
        return image;
    }
    bool metadataRead_ = false;
    bool finished_ = false;
    qint64 origin_ = 0;
    quint64 maximumPixels_ = 0;
    quint64 workingBytes_ = 0;
    QSize size_;
    QSize sensorSize_;
    QSize scaledSize_;
    int count_ = 0;
    bool defaultPreviewPreferred_ = false;
    std::array<int, LIBRAW_THUMBNAIL_MAXCOUNT> order_{};
    QString previewPath_ = QStringLiteral("embedded preview first; progressive RAW development");
    std::unique_ptr<Native> native_;
};
} // namespace

extern "C" Q_DECL_EXPORT QImageIOHandler* licasaCreateImageHandler() { return new RawHandler; }
