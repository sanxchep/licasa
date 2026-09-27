#include "imaging/image_decode_contract.h"

#include <QColorSpace>
#include <QImageIOHandler>
#include <QPainter>
#include <QThread>
#include <QtEndian>

#include <libheif/heif.h>
#include <libheif/heif_sequences.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

namespace {
namespace Contract = Licasa::ImageDecodeContract;
constexpr quint64 metadataBudget = 4 * 1024 * 1024;
constexpr qsizetype maximumMotionXmpBytes = 1024 * 1024;
constexpr int maximumMetadataBlocks = 64;
constexpr int maximumSequenceFrames = 100000;
using Context = std::unique_ptr<heif_context, decltype(&heif_context_free)>;
using Handle = std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)>;
using Track = std::unique_ptr<heif_track, decltype(&heif_track_release)>;
using Pixels = std::unique_ptr<heif_image, decltype(&heif_image_release)>;
using Options = std::unique_ptr<heif_decoding_options, decltype(&heif_decoding_options_free)>;

int decodeWorkerLimit()
{
    const int available = std::max(1, QThread::idealThreadCount());
    bool validOverride = false;
    const int override = qEnvironmentVariableIntValue("LICASA_HEIF_DECODE_WORKERS", &validOverride);
    return validOverride ? std::clamp(override, 0, available) : available;
}

bool heifSignature(QIODevice* device)
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
    // Do not capture AVIF: its maintained AV1 backend has a separate plugin.
    bool compatible = false;
    for (qsizetype offset = 8; offset + 4 <= std::min<qsizetype>(length, header.size());
         offset += 4) {
        if (offset == 12) {
            continue; // minor version, not a brand
        }
        const auto brand = header.mid(offset, 4);
        if (brand == "avif" || brand == "avis") {
            return false;
        }
        if (brand == "heic" || brand == "heix" || brand == "hevc" || brand == "hevx" ||
            brand == "mif1" || brand == "msf1") {
            compatible = true;
        }
    }
    return compatible;
}

const heif_reader& deviceReader()
{
    static const heif_reader reader = [] {
        heif_reader value{};
        value.reader_api_version = 1;
        value.get_position = [](void* data) -> int64_t {
            return static_cast<QIODevice*>(data)->pos();
        };
        value.read = [](void* bytes, size_t count, void* data) -> int {
            auto* device = static_cast<QIODevice*>(data);
            if (Contract::cancelled(device)) {
                return -1;
            }
            const qint64 position = device->pos();
            const qint64 size = device->size();
            if (count > size_t(std::numeric_limits<qint64>::max()) || position < 0 ||
                position > size || quint64(count) > quint64(size - position)) {
                return -1;
            }
            return device->read(static_cast<char*>(bytes), qint64(count)) == qint64(count) ? 0 : -1;
        };
        value.seek = [](int64_t position, void* data) -> int {
            auto* device = static_cast<QIODevice*>(data);
            return !Contract::cancelled(device) && position >= 0 && position <= device->size() &&
                           device->seek(position)
                       ? 0
                       : -1;
        };
        value.wait_for_file_size = [](int64_t size, void* data) {
            return size >= 0 && size <= static_cast<QIODevice*>(data)->size()
                       ? heif_reader_grow_status_size_reached
                       : heif_reader_grow_status_size_beyond_eof;
        };
        return value;
    }();
    return reader;
}

class HeifHandler final : public QImageIOHandler {
  public:
    ~HeifHandler() override
    {
        track_.reset();
        handle_.reset();
        context_.reset();
        if (initialized_) {
            heif_deinit();
        }
    }

    bool canRead() const override
    {
        const bool readable = !readComplete_ && (handle_ || track_ || heifSignature(device()));
        if (readable && format().isEmpty()) {
            setFormat("heic");
        }
        return readable;
    }
    int imageCount() const override
    {
        if (!const_cast<HeifHandler*>(this)->readMetadata()) {
            return 0;
        }
        return sequencePresent_ ? knownFrameCount_ : 1;
    }
    int currentImageNumber() const override { return frameNumber_; }
    int nextImageDelay() const override { return sequencePresent_ ? delayMs_ : 0; }
    int loopCount() const override
    {
        if (!const_cast<HeifHandler*>(this)->readMetadata() || !sequencePresent_) {
            return 0;
        }
        if (sequenceRepetitions_ == heif_sequence_track_number_of_repetitions_infinite) {
            return -1;
        }
        if (sequenceRepetitions_ <= 1) {
            return 0;
        }
        return int(
            std::min<quint32>(sequenceRepetitions_ - 1u, quint32(std::numeric_limits<int>::max())));
    }
    bool jumpToNextImage() override
    {
        if (!readMetadata()) {
            return false;
        }
        const int next = frameNumber_ < 0 ? 0 : frameNumber_ + 1;
        if (next < 0 || next >= maximumSequenceFrames ||
            (knownFrameCount_ > 0 && next >= knownFrameCount_)) {
            return false;
        }
        pendingFrame_ = next;
        readComplete_ = false;
        return true;
    }
    bool jumpToImage(int number) override
    {
        if (!readMetadata() || number < 0 || number >= maximumSequenceFrames ||
            (knownFrameCount_ > 0 && number >= knownFrameCount_)) {
            return false;
        }
        if (!sequencePresent_ && number != 0) {
            return false;
        }
        pendingFrame_ = number;
        readComplete_ = false;
        return true;
    }
    bool supportsOption(ImageOption option) const override
    {
        return option == Size || option == ScaledSize || option == ImageFormat ||
               option == Description || option == Animation;
    }
    QVariant option(ImageOption option) const override
    {
        if (option == ScaledSize) {
            return scaledSize_;
        }
        if (option == ImageFormat) {
            return QImage::Format_RGBA8888;
        }
        if (!const_cast<HeifHandler*>(this)->readMetadata()) {
            return {};
        }
        if (option == Size) {
            return size_;
        }
        if (option == Animation) {
            return sequencePresent_;
        }
        if (option == Description) {
            const heif_decoder_descriptor* decoder = nullptr;
            const int count = heif_get_decoder_descriptors(heif_compression_HEVC, &decoder, 1);
            const QString codec =
                count == 1 && decoder
                    ? QString::fromLatin1(heif_decoder_descriptor_get_name(decoder))
                    : QStringLiteral("HEVC decoder unavailable");
            QString description =
                QStringLiteral(
                    "Backend: libheif %1; %2\n\nPreviewPath: %3\n\nBackendPixelLimit: %4")
                    .arg(QString::fromLatin1(heif_get_version()), codec, previewPath_)
                    .arg(heif_get_global_security_limits()->max_image_size_pixels);
            description += xmpDescriptionFields();
            description += heifExifDescriptionFields();
            return description;
        }
        return {};
    }
    void setOption(ImageOption option, const QVariant& value) override
    {
        if (option == ScaledSize) {
            scaledSize_ = value.toSize();
        }
    }

    bool read(QImage* output) override
    {
        if (device()) {
            device()->setProperty("_licasaNativeRasterPixels", QVariant{});
        }
        if (!output || readComplete_ || !readMetadata() || cancelled()) {
            return false;
        }
        const QSize target = scaledSize_.isValid() && !scaledSize_.isEmpty() ? scaledSize_ : size_;
        if (!Contract::allows(target, pixelBudget_)) {
            return limitFailure();
        }
        Options options(heif_decoding_options_alloc(), heif_decoding_options_free);
        if (!options) {
            return fail("HEIF decoding options could not be allocated.");
        }
        options->strict_decoding = 1;
        options->num_codec_threads = quint64(size_.width()) * size_.height() >= 4'000'000
                                         ? std::max(1, decodeWorkerLimit())
                                         : 1;
        options->convert_hdr_to_8bit = 1;
        options->progress_user_data = this;
        options->cancel_decoding = [](void* data) {
            return static_cast<HeifHandler*>(data)->cancelled() ? 1 : 0;
        };

        if (sequencePresent_) {
            // libheif otherwise applies repeated edit lists itself. Infinite
            // repetition would make iteration never reach End_of_sequence; the
            // Qt animation controller owns looping instead.
            options->ignore_sequence_editlist = 1;
            return readSequence(target, options.get(), output);
        }

        readComplete_ = true;
        Handle thumbnail = suitableThumbnail(target);
        if (thumbnail) {
            previewPath_ = QStringLiteral("embedded thumbnail");
            configureColor(thumbnail.get(), options.get());
            return readSingle(thumbnail.get(), target, options.get(), output);
        }
        configureColor(handle_.get(), options.get());
        heif_image_tiling tiling{};
        tiling.version = 1;
        const auto tileError = heif_image_handle_get_image_tiling(handle_.get(), 1, &tiling);
        if (tileError.code == heif_error_Ok && (tiling.num_columns > 1 || tiling.num_rows > 1)) {
            previewPath_ = QStringLiteral("streamed grid tiles");
            return readGrid(tiling, target, options.get(), output);
        }
        // A single HEVC-coded frame has no native reduced-resolution decoding
        // in libde265. Admit its working raster explicitly; never disguise a
        // full native decode as an efficient scaled one in diagnostics.
        previewPath_ = QStringLiteral("single coded image (full native raster required)");
        return readSingle(handle_.get(), target, options.get(), output);
    }

  private:
    QString xmpDescriptionFields() const
    {
        // App-private bridge for the lazy Motion Photo probe. Description is a
        // newline-delimited key/value channel, so raw XML is never inserted
        // directly; canonical Base64 keeps arbitrary XMP bytes on one line.
        if (!handle_ || sequencePresent_) {
            return {};
        }

        const auto errorField = [](QStringView message) {
            return QStringLiteral("\n\nLicasaXmpError: %1").arg(message.toString());
        };

        const int blockCount =
            heif_image_handle_get_number_of_metadata_blocks(handle_.get(), nullptr);
        if (blockCount <= 0) {
            return {};
        }
        if (blockCount > maximumMetadataBlocks) {
            return errorField(QStringLiteral("HEIF metadata exceeds the 64-block probe limit"));
        }

        heif_item_id ids[maximumMetadataBlocks]{};
        const int listed = heif_image_handle_get_list_of_metadata_block_IDs(
            handle_.get(), nullptr, ids, maximumMetadataBlocks);
        if (listed != blockCount) {
            return errorField(QStringLiteral("HEIF metadata block enumeration was incomplete"));
        }

        QByteArray xmp;
        int xmpBlocks = 0;
        for (int index = 0; index < listed; ++index) {
            if (cancelled()) {
                return errorField(QStringLiteral("HEIF XMP metadata probing was cancelled"));
            }

            const char* type = heif_image_handle_get_metadata_type(handle_.get(), ids[index]);
            const bool explicitXmp = type && std::strcmp(type, "XMP") == 0;
            const char* contentType =
                heif_image_handle_get_metadata_content_type(handle_.get(), ids[index]);
            const bool rdfMime =
                contentType && std::strcmp(contentType, "application/rdf+xml") == 0;
            if (!explicitXmp && !rdfMime) {
                continue;
            }

            if (++xmpBlocks != 1) {
                return errorField(QStringLiteral("HEIF contains multiple XMP metadata blocks"));
            }

            const size_t byteCount = heif_image_handle_get_metadata_size(handle_.get(), ids[index]);
            if (byteCount > size_t(maximumMotionXmpBytes) ||
                byteCount > size_t(std::numeric_limits<qsizetype>::max())) {
                return errorField(
                    QStringLiteral("HEIF XMP metadata exceeds the 1 MiB probe limit"));
            }

            xmp.resize(qsizetype(byteCount));
            const heif_error metadataError =
                heif_image_handle_get_metadata(handle_.get(), ids[index], xmp.data());
            if (metadataError.code != heif_error_Ok) {
                return errorField(QStringLiteral("HEIF XMP metadata could not be read"));
            }
        }

        if (xmpBlocks == 0) {
            return {};
        }
        return QStringLiteral("\n\nLicasaXmpBase64: %1").arg(QString::fromLatin1(xmp.toBase64()));
    }

    QString heifExifDescriptionFields() const
    {
        // HEIF Exif metadata items are returned by libheif as a 4-byte
        // big-endian TIFF-header offset followed by the Exif payload. Keep the
        // raw bounded item here; the shared Apple parser validates the offset,
        // TIFF structure and MakerNote on the PhotoAssetProbe worker thread.
        if (!handle_ || sequencePresent_ || !device() ||
            !device()->property(Contract::appleLivePhotoExifProbeProperty).toBool()) {
            return {};
        }
        const auto errorField = [](QStringView message) {
            return QStringLiteral("\n\nLicasaHeifExifError: %1").arg(message.toString());
        };
        const int blockCount =
            heif_image_handle_get_number_of_metadata_blocks(handle_.get(), "Exif");
        if (blockCount <= 0) {
            return {};
        }
        if (blockCount != 1) {
            return errorField(QStringLiteral("HEIF contains multiple Exif metadata blocks"));
        }

        heif_item_id id = 0;
        const int listed =
            heif_image_handle_get_list_of_metadata_block_IDs(handle_.get(), "Exif", &id, 1);
        if (listed != 1) {
            return errorField(
                QStringLiteral("HEIF Exif metadata block enumeration was incomplete"));
        }
        if (cancelled()) {
            return errorField(QStringLiteral("HEIF Exif metadata probing was cancelled"));
        }

        constexpr size_t maximumAppleExifBytes = 1024 * 1024 + 4;
        const size_t byteCount = heif_image_handle_get_metadata_size(handle_.get(), id);
        if (byteCount < 4 || byteCount > maximumAppleExifBytes ||
            byteCount > size_t(std::numeric_limits<qsizetype>::max())) {
            return errorField(
                QStringLiteral("HEIF Exif metadata exceeds the bounded Apple probe limit"));
        }

        QByteArray exif(qsizetype(byteCount), Qt::Uninitialized);
        const heif_error metadataError =
            heif_image_handle_get_metadata(handle_.get(), id, exif.data());
        if (metadataError.code != heif_error_Ok) {
            return errorField(QStringLiteral("HEIF Exif metadata could not be read"));
        }
        if (cancelled()) {
            return errorField(QStringLiteral("HEIF Exif metadata probing was cancelled"));
        }
        return QStringLiteral("\n\nLicasaHeifExifBase64: %1")
            .arg(QString::fromLatin1(exif.toBase64()));
    }

    bool fail(const QString& message)
    {
        if (device()) {
            device()->setProperty(Contract::errorProperty, message);
        }
        return false;
    }
    bool fail(const heif_error& error)
    {
        return fail(QStringLiteral("HEIF: ") +
                    (error.message
                         ? QString::fromUtf8(error.message, int(strnlen(error.message, 2048)))
                         : QStringLiteral("The image could not be decoded.")));
    }
    bool limitFailure()
    {
        return fail(
            QStringLiteral("HEIF cannot decode this raster within the selected image limit. "
                           "A sufficiently large embedded preview or bounded grid is required."));
    }
    bool cancelled() const { return cancelFlag_ && cancelFlag_->load(std::memory_order_relaxed); }
    bool configureContext()
    {
        context_.reset(heif_context_alloc());
        if (!context_) {
            return fail("HEIF context could not be allocated.");
        }
        auto limits = *heif_get_global_security_limits();
        limits.max_color_profile_size =
            std::min<quint32>(limits.max_color_profile_size, metadataBudget);
        const quint64 rasterWorkBudget = pixelBudget_ > std::numeric_limits<quint64>::max() / 16
                                             ? std::numeric_limits<quint64>::max()
                                             : pixelBudget_ * 16;
        // Metadata discovery must remain possible even when the caller's raster
        // budget is intentionally tiny. Keep that parse bounded independently;
        // read() still rejects an over-budget target before libheif decodes pixels.
        const quint64 workBudget = std::max<quint64>(metadataBudget, rasterWorkBudget);
        limits.max_total_memory = std::min<uint64_t>(limits.max_total_memory, workBudget);
        limits.max_memory_block_size = std::min<uint64_t>(limits.max_memory_block_size, workBudget);
        limits.max_sequence_frames =
            std::min<uint32_t>(limits.max_sequence_frames, maximumSequenceFrames);
        auto error = heif_context_set_security_limits(context_.get(), &limits);
        if (error.code != heif_error_Ok) {
            return fail(error);
        }
        heif_context_set_max_decoding_threads(context_.get(), 0);
        if (!device()->seek(0)) {
            return fail("HEIF input could not be rewound.");
        }
        error = heif_context_read_from_reader(context_.get(), &deviceReader(), device(), nullptr);
        return error.code == heif_error_Ok ? true : fail(error);
    }
    bool acquireSequenceTrack(bool validateMetadata)
    {
        heif_track* raw = heif_context_get_track(context_.get(), sequenceTrackId_);
        Track candidate(raw, heif_track_release);
        if (!candidate) {
            return fail("HEIF sequence does not contain a readable visual track.");
        }
        const heif_track_type type = heif_track_get_track_handler_type(candidate.get());
        if (type != heif_track_type_image_sequence && type != heif_track_type_video) {
            return fail("HEIF sequence track is not visual.");
        }
        uint16_t width = 0;
        uint16_t height = 0;
        const auto error = heif_track_get_image_resolution(candidate.get(), &width, &height);
        if (error.code != heif_error_Ok) {
            return fail(error);
        }
        const QSize actual{int(width), int(height)};
        if (actual.isEmpty()) {
            return fail("HEIF sequence declares invalid image dimensions.");
        }
        if (validateMetadata && actual != size_) {
            return fail("HEIF sequence dimensions changed while seeking.");
        }
        track_ = std::move(candidate);
        sequenceCursor_ = 0;
        return true;
    }
    bool resetSequenceDecoder()
    {
        track_.reset();
        handle_.reset();
        context_.reset();
        if (!configureContext()) {
            return false;
        }
        if (!heif_context_has_sequence(context_.get())) {
            return fail("HEIF sequence disappeared while seeking.");
        }
        return acquireSequenceTrack(true);
    }
    bool readMetadata()
    {
        if (metadataAttempted_) {
            return sequencePresent_ ? bool(track_) : bool(handle_);
        }
        metadataAttempted_ = true;
        if (!canRead() || device()->isSequential()) {
            return fail("HEIF requires a seekable local image.");
        }
        pixelBudget_ = Contract::pixelBudget(device());
        cancelFlag_ = reinterpret_cast<const std::atomic_bool*>(
            device()->property(Contract::cancellationProperty).value<quintptr>());
        if (cancelled()) {
            return false;
        }
        auto error = heif_init(nullptr);
        if (error.code != heif_error_Ok) {
            return fail(error);
        }
        initialized_ = true;
        if (!configureContext()) {
            return false;
        }

        if (heif_context_has_sequence(context_.get())) {
            Track candidate(heif_context_get_track(context_.get(), 0), heif_track_release);
            if (candidate) {
                const heif_track_type type = heif_track_get_track_handler_type(candidate.get());
                if (type == heif_track_type_image_sequence || type == heif_track_type_video) {
                    sequenceTrackId_ = heif_track_get_id(candidate.get());
                    uint16_t width = 0;
                    uint16_t height = 0;
                    error = heif_track_get_image_resolution(candidate.get(), &width, &height);
                    if (error.code != heif_error_Ok) {
                        return fail(error);
                    }
                    size_ = QSize(int(width), int(height));
                    if (size_.isEmpty()) {
                        return fail("HEIF sequence declares invalid image dimensions.");
                    }
                    sequenceTimescale_ = heif_track_get_timescale(candidate.get());
                    if (sequenceTimescale_ == 0) {
                        return fail("HEIF sequence declares an invalid timescale.");
                    }
                    sequenceRepetitions_ = heif_track_get_number_of_repetitions(candidate.get());
                    sequencePresent_ = true;
                    track_ = std::move(candidate);
                    previewPath_ = QStringLiteral(
                        "HEIF image sequence (full native raster required per frame)");
                    if (format() == "heif" || format() == "hif" || format() == "heifs") {
                        setFormat("heifs");
                    } else if (format().isEmpty() || format() == "heic") {
                        setFormat("heics");
                    }
                    return true;
                }
            }
        }

        heif_image_handle* handle = nullptr;
        error = heif_context_get_primary_image_handle(context_.get(), &handle);
        if (error.code != heif_error_Ok) {
            return fail(error);
        }
        handle_.reset(handle);
        size_ = {heif_image_handle_get_width(handle), heif_image_handle_get_height(handle)};
        if (size_.isEmpty()) {
            handle_.reset();
            return fail("HEIF declares invalid image dimensions.");
        }
        device()->setProperty(Contract::embeddedPreviewSizeProperty, largestThumbnailSize());
        return true;
    }
    int sequenceDelayMs(uint32_t duration) const
    {
        if (duration == 0 || sequenceTimescale_ == 0) {
            return 100;
        }
        const long double milliseconds = static_cast<long double>(duration) * 1000.0L /
                                         static_cast<long double>(sequenceTimescale_);
        if (milliseconds <= 0.0L) {
            return 100;
        }
        return int(std::clamp(milliseconds, 10.0L, 60000.0L));
    }
    bool readSequence(const QSize& target, const heif_decoding_options* options, QImage* output)
    {
        if (!track_ || !Contract::allows(size_, pixelBudget_)) {
            return limitFailure();
        }
        if (pendingFrame_ < 0 || pendingFrame_ >= maximumSequenceFrames ||
            (knownFrameCount_ > 0 && pendingFrame_ >= knownFrameCount_)) {
            readComplete_ = true;
            return false;
        }
        if (pendingFrame_ < sequenceCursor_ && !resetSequenceDecoder()) {
            return false;
        }

        auto* limits = heif_context_get_security_limits(context_.get());
        limits->max_image_size_pixels =
            std::min<uint64_t>(limits->max_image_size_pixels, pixelBudget_);
        for (;;) {
            if (cancelled()) {
                return fail("HEIF sequence decoding was cancelled.");
            }
            heif_image* decoded = nullptr;
            const auto error = heif_track_decode_next_image(
                track_.get(), &decoded, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, options);
            Pixels pixels(decoded, heif_image_release);
            if (error.code == heif_error_End_of_sequence) {
                knownFrameCount_ = sequenceCursor_;
                readComplete_ = true;
                return false;
            }
            if (error.code != heif_error_Ok) {
                return fail(error);
            }
            if (sequenceCursor_ >= maximumSequenceFrames) {
                return fail("HEIF sequence contains too many frames.");
            }

            const int decodedFrame = sequenceCursor_++;
            if (decodedFrame != pendingFrame_) {
                continue;
            }

            delayMs_ = sequenceDelayMs(heif_image_get_duration(pixels.get()));
            sourceColor_ = QColorSpace::SRgb;
            QImage image = wrapPixels(std::move(pixels));
            if (image.isNull()) {
                return fail("HEIF sequence returned invalid pixel dimensions or stride.");
            }
            if (image.size() != size_) {
                return fail("HEIF sequence frame dimensions differ from the track.");
            }
            if (image.size() != target) {
                image = image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
            if (image.isNull() || cancelled()) {
                return false;
            }

            frameNumber_ = decodedFrame;
            pendingFrame_ = decodedFrame + 1;
            readComplete_ = false;
            *output = std::move(image);
            return true;
        }
    }
    QSize largestThumbnailSize() const
    {
        QSize largest;
        quint64 largestPixels = 0;
        heif_item_id ids[64]{};
        const int count = heif_image_handle_get_list_of_thumbnail_IDs(handle_.get(), ids, 64);
        for (int index = 0; index < count && index < 64; ++index) {
            heif_image_handle* pointer = nullptr;
            const auto error = heif_image_handle_get_thumbnail(handle_.get(), ids[index], &pointer);
            Handle item(pointer, heif_image_handle_release);
            if (error.code != heif_error_Ok || !item) {
                continue;
            }
            const QSize size(heif_image_handle_get_width(item.get()),
                             heif_image_handle_get_height(item.get()));
            if (!Contract::allows(size, pixelBudget_) ||
                qAbs(qint64(size.width()) * size_.height() -
                     qint64(size.height()) * size_.width()) >
                    std::max(size_.width(), size_.height())) {
                continue;
            }
            const quint64 pixels = quint64(size.width()) * size.height();
            if (pixels > largestPixels) {
                largestPixels = pixels;
                largest = size;
            }
        }
        return largest;
    }

    Handle suitableThumbnail(const QSize& target)
    {
        Handle chosen(nullptr, heif_image_handle_release);
        if (target == size_) {
            return chosen;
        }
        heif_item_id ids[64]{};
        const int count = heif_image_handle_get_list_of_thumbnail_IDs(handle_.get(), ids, 64);
        quint64 chosenPixels = std::numeric_limits<quint64>::max();
        for (int index = 0; index < count && index < 64; ++index) {
            heif_image_handle* pointer = nullptr;
            const auto error = heif_image_handle_get_thumbnail(handle_.get(), ids[index], &pointer);
            Handle item(pointer, heif_image_handle_release);
            if (error.code != heif_error_Ok || !item) {
                continue;
            }
            const QSize size(heif_image_handle_get_width(item.get()),
                             heif_image_handle_get_height(item.get()));
            if (!Contract::allows(size, pixelBudget_) || size.width() < target.width() ||
                size.height() < target.height()) {
                continue;
            }
            // Do not stretch a thumbnail with a different crop/aspect ratio.
            const qint64 aspectError =
                qAbs(qint64(size.width()) * size_.height() - qint64(size.height()) * size_.width());
            if (aspectError > std::max(size_.width(), size_.height())) {
                continue;
            }
            const quint64 pixels = quint64(size.width()) * size.height();
            if (pixels < chosenPixels) {
                chosenPixels = pixels;
                chosen = std::move(item);
            }
        }
        return chosen;
    }
    void configureColor(heif_image_handle* source, heif_decoding_options* options)
    {
        // Parse once per selected image, not once per tile; an embedded preview
        // may carry a different profile than its primary image.
        sourceColor_ = QColorSpace::SRgb;
        const size_t profileSize = heif_image_handle_get_raw_color_profile_size(source);
        if (profileSize > 0 && profileSize <= metadataBudget) {
            QByteArray profile(qsizetype(profileSize), Qt::Uninitialized);
            if (heif_image_handle_get_raw_color_profile(source, profile.data()).code ==
                heif_error_Ok) {
                const auto color = QColorSpace::fromIccProfile(profile);
                if (color.isValid()) {
                    sourceColor_ = color;
                    // Keep source RGB coordinates for Qt's ICC transform.
                    options->output_image_nclx_profile_passthrough = 1;
                }
            }
        }
    }
    QImage wrapPixels(Pixels pixels)
    {
        if (!pixels) {
            return {};
        }
        const QSize size(heif_image_get_width(pixels.get(), heif_channel_interleaved),
                         heif_image_get_height(pixels.get(), heif_channel_interleaved));
        int stride = 0;
        const auto* plane =
            heif_image_get_plane_readonly(pixels.get(), heif_channel_interleaved, &stride);
        if (!Contract::allows(size, pixelBudget_) || !plane || stride < qint64(size.width()) * 4) {
            return {};
        }
        QImage image(
            plane, size.width(), size.height(), stride, QImage::Format_RGBA8888,
            [](void* data) { heif_image_release(static_cast<heif_image*>(data)); }, pixels.get());
        if (image.isNull()) {
            return {};
        }
        pixels.release();
        peakNativePixels_ = std::max(peakNativePixels_, quint64(size.width()) * size.height());
        if (device()) {
            device()->setProperty("_licasaNativeRasterPixels",
                                  QVariant::fromValue(peakNativePixels_));
        }
        // NCLX-only images were converted to sRGB by libheif. ICC-tagged
        // images retain source RGB above and are converted here, including on
        // older Qt Quick renderers that do not color-manage image textures.
        image.setColorSpace(sourceColor_);
        if (sourceColor_ != QColorSpace(QColorSpace::SRgb)) {
            image.convertToColorSpace(QColorSpace::SRgb);
        }
        return image;
    }
    bool readSingle(heif_image_handle* handle, const QSize& target,
                    const heif_decoding_options* options, QImage* output)
    {
        const QSize native(heif_image_handle_get_width(handle),
                           heif_image_handle_get_height(handle));
        if (!Contract::allows(native, pixelBudget_)) {
            return limitFailure();
        }
        auto* limits = heif_context_get_security_limits(context_.get());
        limits->max_image_size_pixels =
            std::min<uint64_t>(limits->max_image_size_pixels, pixelBudget_);
        heif_image* decoded = nullptr;
        const auto error = heif_decode_image(handle, &decoded, heif_colorspace_RGB,
                                             heif_chroma_interleaved_RGBA, options);
        Pixels pixels(decoded, heif_image_release);
        if (error.code != heif_error_Ok) {
            return fail(error);
        }
        if (cancelled()) {
            return false;
        }
        QImage image = wrapPixels(std::move(pixels));
        if (image.isNull()) {
            return fail("HEIF returned invalid pixel dimensions or stride.");
        }
        if (image.size() != target) {
            image = image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        if (image.isNull() || cancelled()) {
            return false;
        }
        *output = std::move(image);
        return true;
    }
    struct GridTile {
        Pixels pixels{nullptr, heif_image_release};
    };
    enum class ParallelGridResult { NotUsed, Ready, Failed };

    ParallelGridResult decodeGridTilesParallel(const heif_image_tiling& grid,
                                               const heif_decoding_options* options,
                                               const heif_security_limits* limits,
                                               std::vector<GridTile>* tiles)
    {
        const quint64 tileCount = quint64(grid.num_columns) * grid.num_rows;
        const qint64 inputBytes = device()->size();
        // Each worker owns its reader cursor and HEIF context. Copying a bounded
        // compressed source avoids concurrent seeks on the caller's QIODevice.
        // Keep the serial path for unusually large files and small grids.
        if (tileCount < 8 || tileCount > 512 || inputBytes <= 0 || inputBytes > 256 * 1024 * 1024 ||
            quint64(inputBytes) > pixelBudget_ * 4 ||
            quint64(grid.tile_width) * grid.tile_height * tileCount > pixelBudget_ * 2 ||
            decodeWorkerLimit() < 4 || !device()->seek(0)) {
            return ParallelGridResult::NotUsed;
        }

        const QByteArray compressed = device()->read(inputBytes);
        if (compressed.size() != inputBytes) {
            return ParallelGridResult::NotUsed;
        }
        if (cancelled()) {
            return ParallelGridResult::Failed;
        }

        std::vector<GridTile> decodedTiles(static_cast<size_t>(tileCount));
        const int workers = std::min<int>(decodeWorkerLimit(), int(tileCount));
        std::vector<QString> workerErrors(static_cast<size_t>(workers));
        std::atomic<quint64> nextTile{0};
        std::atomic_bool stopped{false};
        const auto decodeTiles = [&](int workerIndex) {
            const auto reject = [&](const heif_error& error) {
                workerErrors[size_t(workerIndex)] =
                    error.message ? QString::fromUtf8(error.message)
                                  : QStringLiteral("HEIF tile decode failed.");
                stopped.store(true, std::memory_order_relaxed);
            };
            Context context(heif_context_alloc(), heif_context_free);
            if (!context) {
                workerErrors[size_t(workerIndex)] =
                    QStringLiteral("HEIF tile context could not be allocated.");
                stopped.store(true, std::memory_order_relaxed);
                return;
            }
            const auto error = heif_context_set_security_limits(context.get(), limits);
            if (error.code != heif_error_Ok) {
                reject(error);
                return;
            }
            heif_context_set_max_decoding_threads(context.get(), 0);
            const auto readError = heif_context_read_from_memory_without_copy(
                context.get(), compressed.constData(), size_t(compressed.size()), nullptr);
            if (readError.code != heif_error_Ok) {
                reject(readError);
                return;
            }
            heif_image_handle* rawHandle = nullptr;
            const auto handleError =
                heif_context_get_primary_image_handle(context.get(), &rawHandle);
            Handle handle(rawHandle, heif_image_handle_release);
            if (handleError.code != heif_error_Ok || !handle ||
                heif_image_handle_get_width(handle.get()) != size_.width() ||
                heif_image_handle_get_height(handle.get()) != size_.height()) {
                reject(handleError.code != heif_error_Ok
                           ? handleError
                           : heif_error{heif_error_Invalid_input, heif_suberror_Unspecified,
                                        "HEIF grid dimensions changed while reading."});
                return;
            }
            heif_image_tiling workerGrid{};
            workerGrid.version = 1;
            const auto gridError = heif_image_handle_get_image_tiling(handle.get(), 1, &workerGrid);
            if (gridError.code != heif_error_Ok || workerGrid.num_columns != grid.num_columns ||
                workerGrid.num_rows != grid.num_rows || workerGrid.tile_width != grid.tile_width ||
                workerGrid.tile_height != grid.tile_height ||
                workerGrid.left_offset != grid.left_offset ||
                workerGrid.top_offset != grid.top_offset) {
                reject(gridError.code != heif_error_Ok
                           ? gridError
                           : heif_error{heif_error_Invalid_input, heif_suberror_Unspecified,
                                        "HEIF grid layout changed while reading."});
                return;
            }
            Options workerOptions(heif_decoding_options_alloc(), heif_decoding_options_free);
            if (!workerOptions) {
                workerErrors[size_t(workerIndex)] =
                    QStringLiteral("HEIF tile options could not be allocated.");
                stopped.store(true, std::memory_order_relaxed);
                return;
            }
            heif_decoding_options_copy(workerOptions.get(), options);
            workerOptions->num_codec_threads = 1;
            while (!stopped.load(std::memory_order_relaxed) && !cancelled()) {
                const quint64 index = nextTile.fetch_add(1, std::memory_order_relaxed);
                if (index >= tileCount) {
                    break;
                }
                heif_image* decoded = nullptr;
                const auto tileError = heif_image_handle_decode_image_tile(
                    handle.get(), &decoded, heif_colorspace_RGB, heif_chroma_interleaved_RGBA,
                    workerOptions.get(), quint32(index % grid.num_columns),
                    quint32(index / grid.num_columns));
                if (tileError.code != heif_error_Ok) {
                    if (decoded) {
                        heif_image_release(decoded);
                    }
                    reject(tileError);
                    return;
                }
                decodedTiles[size_t(index)].pixels.reset(decoded);
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(size_t(workers));
        bool launched = true;
        try {
            for (int worker = 0; worker < workers; ++worker) {
                threads.emplace_back(decodeTiles, worker);
            }
        } catch (const std::system_error&) {
            stopped.store(true, std::memory_order_relaxed);
            launched = false;
        }
        for (auto& thread : threads) {
            thread.join();
        }
        if (cancelled()) {
            return ParallelGridResult::Failed;
        }
        if (!launched) {
            return ParallelGridResult::NotUsed;
        }
        for (const auto& error : workerErrors) {
            if (!error.isEmpty()) {
                fail(error);
                return ParallelGridResult::Failed;
            }
        }
        *tiles = std::move(decodedTiles);
        return ParallelGridResult::Ready;
    }
    bool readGrid(const heif_image_tiling& grid, const QSize& target,
                  const heif_decoding_options* options, QImage* output)
    {
        if (grid.number_of_extra_dimensions != 0 || grid.num_columns == 0 || grid.num_rows == 0 ||
            grid.tile_width == 0 || grid.tile_height == 0 ||
            grid.tile_width > quint32(std::numeric_limits<int>::max()) ||
            grid.tile_height > quint32(std::numeric_limits<int>::max()) ||
            quint64(grid.num_columns) * grid.num_rows >
                heif_get_global_security_limits()->max_number_of_tiles ||
            grid.image_width != quint32(size_.width()) ||
            grid.image_height != quint32(size_.height()) ||
            !Contract::allows(QSize(int(grid.tile_width), int(grid.tile_height)), pixelBudget_)) {
            return fail("HEIF grid dimensions exceed the decoder's resource limits.");
        }
        if (!allocateImage(target, QImage::Format_RGBA8888, output)) {
            return limitFailure();
        }
        output->fill(Qt::transparent);
        output->setColorSpace(QColorSpace::SRgb);
        QPainter painter(output);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        auto* limits = heif_context_get_security_limits(context_.get());
        limits->max_image_size_pixels =
            std::min<uint64_t>(limits->max_image_size_pixels, pixelBudget_);
        const auto drawTile = [&](Pixels pixels, quint32 column, quint32 row) {
            const QImage tile = wrapPixels(std::move(pixels));
            if (tile.isNull()) {
                return fail("HEIF returned an invalid image tile.");
            }
            const qint64 left = qint64(column) * grid.tile_width - grid.left_offset;
            const qint64 top = qint64(row) * grid.tile_height - grid.top_offset;
            const QRectF destination(double(left) * target.width() / size_.width(),
                                     double(top) * target.height() / size_.height(),
                                     double(tile.width()) * target.width() / size_.width(),
                                     double(tile.height()) * target.height() / size_.height());
            painter.drawImage(destination, tile);
            if (tile.colorSpace().isValid()) {
                output->setColorSpace(tile.colorSpace());
            }
            return true;
        };

        std::vector<GridTile> tiles;
        switch (decodeGridTilesParallel(grid, options, limits, &tiles)) {
        case ParallelGridResult::Ready:
            for (quint64 index = 0; index < tiles.size(); ++index) {
                if (!tiles[size_t(index)].pixels ||
                    !drawTile(std::move(tiles[size_t(index)].pixels),
                              quint32(index % grid.num_columns),
                              quint32(index / grid.num_columns))) {
                    return false;
                }
            }
            return !cancelled();
        case ParallelGridResult::Failed:
            return false;
        case ParallelGridResult::NotUsed:
            break;
        }
        for (quint32 row = 0; row < grid.num_rows; ++row) {
            for (quint32 column = 0; column < grid.num_columns; ++column) {
                if (cancelled()) {
                    return false;
                }
                heif_image* decoded = nullptr;
                const auto error = heif_image_handle_decode_image_tile(
                    handle_.get(), &decoded, heif_colorspace_RGB, heif_chroma_interleaved_RGBA,
                    options, column, row);
                Pixels pixels(decoded, heif_image_release);
                if (error.code != heif_error_Ok) {
                    return fail(error);
                }
                if (!drawTile(std::move(pixels), column, row)) {
                    return false;
                }
            }
        }
        return !cancelled();
    }

    bool initialized_ = false;
    bool metadataAttempted_ = false;
    bool readComplete_ = false;
    bool sequencePresent_ = false;
    quint64 pixelBudget_ = 0;
    quint64 peakNativePixels_ = 0;
    const std::atomic_bool* cancelFlag_ = nullptr;
    Context context_{nullptr, heif_context_free};
    Handle handle_{nullptr, heif_image_handle_release};
    Track track_{nullptr, heif_track_release};
    uint32_t sequenceTrackId_ = 0;
    uint32_t sequenceTimescale_ = 0;
    uint32_t sequenceRepetitions_ = 1;
    int sequenceCursor_ = 0;
    int knownFrameCount_ = 0;
    int frameNumber_ = -1;
    int pendingFrame_ = 0;
    int delayMs_ = 100;
    QSize size_;
    QSize scaledSize_;
    QColorSpace sourceColor_;
    QString previewPath_ = QStringLiteral("not decoded");
};
} // namespace

extern "C" Q_DECL_EXPORT QImageIOHandler* licasaCreateImageHandler() { return new HeifHandler; }
