#include "codec_buffer.h"
#include "imaging/image_decode_contract.h"

#include <QColorSpace>
#include <QImageIOHandler>
#include <QThread>
#include <jxl/cms.h>
#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/thread_parallel_runner.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace {
namespace Contract = Licasa::ImageDecodeContract;
constexpr size_t metadataLimit = 4 * 1024 * 1024;
constexpr qsizetype inputChunk = 64 * 1024;
constexpr qsizetype inputLimit = 1024 * 1024;
constexpr qsizetype largeStillChunk = 4 * 1024 * 1024;
constexpr qint64 wholeStillInputLimit = 256 * 1024 * 1024;
using Decoder = std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)>;
using ParallelRunner = std::unique_ptr<void, decltype(&JxlThreadParallelRunnerDestroy)>;

class JxlHandler final : public QImageIOHandler {
  public:
    bool canRead() const override
    {
        if (failed_ || lastFrame_ || !device() || !device()->isReadable()) {
            return false;
        }
        if (metadataRead_) {
            return true;
        }
        const auto header = device()->peek(12);
        const auto signature = JxlSignatureCheck(
            reinterpret_cast<const uint8_t*>(header.constData()), size_t(header.size()));
        if (signature != JXL_SIG_CODESTREAM && signature != JXL_SIG_CONTAINER) {
            return false;
        }
        if (format().isEmpty()) {
            setFormat("jxl");
        }
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
        if (!const_cast<JxlHandler*>(this)->readMetadata()) {
            return {};
        }
        if (option == Size) {
            return size_;
        }
        if (option == Animation) {
            return bool(info_.have_animation);
        }
        if (option == Description) {
            const auto version = JxlDecoderVersion();
            return QStringLiteral("Backend: libjxl %1.%2.%3\n\nPreviewPath: %4")
                .arg(version / 1000000)
                .arg((version / 1000) % 1000)
                .arg(version % 1000)
                .arg(previewPath_);
        }
        return {};
    }
    void setOption(ImageOption option, const QVariant& value) override
    {
        if (option == ScaledSize) {
            scaledSize_ = value.toSize();
        }
    }
    int imageCount() const override
    {
        if (!const_cast<JxlHandler*>(this)->readMetadata()) {
            return 0;
        }
        // The codestream does not advertise a frame count. Do not scan/decode
        // the whole animation merely to answer a metadata query.
        return info_.have_animation ? knownFrameCount_ : 1;
    }
    int currentImageNumber() const override { return frameNumber_; }
    int nextImageDelay() const override { return delayMs_; }
    int loopCount() const override
    {
        if (!const_cast<JxlHandler*>(this)->readMetadata() || !info_.have_animation) {
            return 0;
        }
        return info_.animation.num_loops == 0
                   ? -1
                   : int(std::min<quint32>(info_.animation.num_loops - 1,
                                           std::numeric_limits<int>::max()));
    }
    bool jumpToNextImage() override { return !failed_ && !lastFrame_; }
    bool jumpToImage(int number) override
    {
        if (number < 0 || failed_ || (knownFrameCount_ > 0 && number >= knownFrameCount_)) {
            return false;
        }
        if (number != frameNumber_ + 1) {
            // libjxl retains only the references required by the codestream.
            // Its skip API reconstructs references without an all-frame cache.
            decoder_.reset();
            input_.clear();
            pendingFrame_ = number;
        }
        lastFrame_ = false;
        return true;
    }

    bool read(QImage* output) override
    {
        if (!output || failed_ || lastFrame_ || !readMetadata() || cancelled()) {
            return false;
        }
        const QSize target = scaledSize_.isValid() && !scaledSize_.isEmpty() ? scaledSize_ : size_;
        if (!Contract::allows(target, pixelBudget_)) {
            return limitFailure();
        }

        const bool usePreview = !info_.have_animation && pendingFrame_ == 0 && frameNumber_ < 0 &&
                                info_.have_preview && target != size_ &&
                                previewSize_.width() >= target.width() &&
                                previewSize_.height() >= target.height() &&
                                qAbs(qint64(previewSize_.width()) * size_.height() -
                                     qint64(previewSize_.height()) * size_.width()) <=
                                    std::max(size_.width(), size_.height());
        const QSize rasterSize = usePreview ? previewSize_ : size_;
        if (!Contract::allows(rasterSize, pixelBudget_)) {
            return limitFailure();
        }
        previewPath_ =
            usePreview ? QStringLiteral("embedded preview")
                       : QStringLiteral(
                             "full native raster required; requested output resized after decode");

        // Float reconstruction, reference frames and scratch space all share
        // one derived ceiling. This is not another user-selectable pixel limit.
        allocationLimit_ =
            size_t(std::min<quint64>(pixelBudget_, std::numeric_limits<size_t>::max() / 64)) * 64;
        if (!decoder_ && !initialize(false, usePreview)) {
            return false;
        }
        QImage raster;
        JxlFrameHeader frame{};
        const JxlPixelFormat pixels{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
        while (!cancelled()) {
            const auto status = process();
            if (status == JXL_DEC_BASIC_INFO) {
                JxlBasicInfo actual{};
                if (JxlDecoderGetBasicInfo(decoder_.get(), &actual) != JXL_DEC_SUCCESS ||
                    actual.xsize != quint32(size_.width()) ||
                    actual.ysize != quint32(size_.height())) {
                    return fail("JPEG XL dimensions changed while reading.");
                }
            } else if (status == JXL_DEC_COLOR_ENCODING) {
                size_t profileSize = 0;
                if (JxlDecoderGetICCProfileSize(decoder_.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                                &profileSize) != JXL_DEC_SUCCESS ||
                    profileSize > metadataLimit) {
                    return fail("JPEG XL color profile exceeds the metadata limit.");
                }
                JxlColorEncoding srgb{};
                srgb.color_space = JXL_COLOR_SPACE_RGB;
                srgb.white_point = JXL_WHITE_POINT_D65;
                srgb.primaries = JXL_PRIMARIES_SRGB;
                srgb.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
                srgb.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
                if (JxlDecoderSetOutputColorProfile(decoder_.get(), &srgb, nullptr, 0) !=
                    JXL_DEC_SUCCESS) {
                    return fail("JPEG XL color conversion is unavailable for this profile.");
                }
            } else if (status == JXL_DEC_FRAME) {
                if (JxlDecoderGetFrameHeader(decoder_.get(), &frame) != JXL_DEC_SUCCESS) {
                    return fail("JPEG XL frame metadata is invalid.");
                }
                if (info_.have_animation) {
                    const long double duration = static_cast<long double>(frame.duration) * 1000 *
                                                 info_.animation.tps_denominator /
                                                 info_.animation.tps_numerator;
                    delayMs_ = int(std::clamp(duration, 10.0L, 60000.0L));
                }
            } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER ||
                       status == JXL_DEC_NEED_PREVIEW_OUT_BUFFER) {
                if ((status == JXL_DEC_NEED_PREVIEW_OUT_BUFFER) != usePreview) {
                    return fail("JPEG XL requested an unexpected output buffer.");
                }
                size_t required = 0;
                const auto result =
                    usePreview ? JxlDecoderPreviewOutBufferSize(decoder_.get(), &pixels, &required)
                               : JxlDecoderImageOutBufferSize(decoder_.get(), &pixels, &required);
                const quint64 expected = quint64(rasterSize.width()) * rasterSize.height() * 4;
                if (result != JXL_DEC_SUCCESS || required != expected ||
                    !allocateRaster(rasterSize, &raster)) {
                    return limitFailure();
                }
                device()->setProperty("_licasaNativeRasterPixels",
                                      QVariant::fromValue(expected / 4));
                const auto setResult =
                    usePreview
                        ? JxlDecoderSetPreviewOutBuffer(decoder_.get(), &pixels, raster.bits(),
                                                        size_t(raster.sizeInBytes()))
                        : JxlDecoderSetImageOutBuffer(decoder_.get(), &pixels, raster.bits(),
                                                      size_t(raster.sizeInBytes()));
                if (setResult != JXL_DEC_SUCCESS) {
                    return fail("JPEG XL output buffer was rejected.");
                }
            } else if (status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_PREVIEW_IMAGE) {
                if (raster.isNull() || cancelled()) {
                    return false;
                }
                frameNumber_ = pendingFrame_;
                if (frameNumber_ == std::numeric_limits<int>::max()) {
                    return fail("JPEG XL has too many frames.");
                }
                pendingFrame_ = frameNumber_ + 1;
                lastFrame_ = usePreview || !info_.have_animation || frame.is_last;
                if (lastFrame_) {
                    knownFrameCount_ = pendingFrame_;
                    decoder_.reset();
                    input_.clear();
                }
                device()->setProperty("_licasaNativePeakBytes",
                                      QVariant::fromValue(quint64(peakAllocation_.load())));
                raster.setColorSpace(QColorSpace::SRgb);
                if (target != rasterSize) {
                    raster = raster.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                }
                if (raster.isNull() || cancelled()) {
                    return false;
                }
                *output = std::move(raster);
                return true;
            } else {
                return fail(allocationRejected_.load()
                                ? "JPEG XL exceeds the selected memory limit."
                                : "JPEG XL is truncated, unsupported or invalid.");
            }
        }
        return fail("JPEG XL decoding was cancelled.");
    }

  private:
    bool cancelled() const { return Contract::cancelled(device()); }
    bool fail(const QString& message)
    {
        failed_ = true;
        decoder_.reset();
        input_.clear();
        if (device()) {
            device()->setProperty(Contract::errorProperty, message);
        }
        return false;
    }
    bool limitFailure()
    {
        return fail("JPEG XL cannot decode this raster within the selected image limit.");
    }

    bool allocateRaster(const QSize& size, QImage* output)
    {
        if (!Contract::allows(size, pixelBudget_)) {
            return false;
        }
        const quint64 bytes = quint64(size.width()) * size.height() * 4;
        const int qtLimit = QImageReader::allocationLimit();
        if (bytes > std::numeric_limits<size_t>::max() ||
            (qtLimit > 0 && bytes > quint64(qtLimit) * 1024 * 1024)) {
            return false;
        }
        if (!Licasa::CodecBuffer::usesMapping(size_t(bytes))) {
            return allocateImage(size, QImage::Format_RGBA8888, output);
        }
        struct Storage {
            void* address;
            size_t bytes;
        };
        auto* storage = new (std::nothrow) Storage{nullptr, size_t(bytes)};
        if (!storage) {
            return false;
        }
        storage->address = Licasa::CodecBuffer::allocate(storage->bytes);
        if (!storage->address) {
            delete storage;
            return false;
        }
        const auto cleanup = [](void* opaque) {
            auto* owned = static_cast<Storage*>(opaque);
            Licasa::CodecBuffer::release(owned->address, owned->bytes);
            delete owned;
        };
        QImage raster(static_cast<uchar*>(storage->address), size.width(), size.height(),
                      qsizetype(size.width()) * 4, QImage::Format_RGBA8888, cleanup, storage);
        if (raster.isNull()) {
            cleanup(storage);
            return false;
        }
        *output = std::move(raster);
        return true;
    }

    struct alignas(std::max_align_t) Allocation {
        size_t size;
    };
    static void* allocate(void* opaque, size_t count) noexcept
    {
        auto& self = *static_cast<JxlHandler*>(opaque);
        if (self.cancelled()) {
            return nullptr;
        }
        if (count > std::numeric_limits<size_t>::max() - sizeof(Allocation)) {
            self.allocationRejected_.store(true, std::memory_order_relaxed);
            return nullptr;
        }
        const size_t total = count + sizeof(Allocation);
        size_t live = self.liveAllocation_.load(std::memory_order_relaxed);
        while (true) {
            if (live > self.allocationLimit_ || total > self.allocationLimit_ - live) {
                self.allocationRejected_.store(true, std::memory_order_relaxed);
                return nullptr;
            }
            if (self.liveAllocation_.compare_exchange_weak(
                    live, live + total, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                break;
            }
        }
        auto* allocation = static_cast<Allocation*>(Licasa::CodecBuffer::allocate(total));
        if (!allocation) {
            self.liveAllocation_.fetch_sub(total, std::memory_order_acq_rel);
            return nullptr;
        }
        allocation->size = total;
        size_t peak = self.peakAllocation_.load(std::memory_order_relaxed);
        while (peak < live + total && !self.peakAllocation_.compare_exchange_weak(
                                          peak, live + total, std::memory_order_relaxed)) {
        }
        return allocation + 1;
    }
    static void release(void* opaque, void* address) noexcept
    {
        if (!address) {
            return;
        }
        auto* allocation = static_cast<Allocation*>(address) - 1;
        const size_t bytes = allocation->size;
        Licasa::CodecBuffer::release(allocation, bytes);
        static_cast<JxlHandler*>(opaque)->liveAllocation_.fetch_sub(bytes,
                                                                    std::memory_order_acq_rel);
    }
    static JxlParallelRetCode run(void* opaque, void* work, JxlParallelRunInit init,
                                  JxlParallelRunFunction function, uint32_t first, uint32_t end)
    {
        auto& self = *static_cast<JxlHandler*>(opaque);
        if (self.cancelled()) {
            return JXL_PARALLEL_RET_RUNNER_ERROR;
        }
        const auto result = init(work, 1);
        if (result != JXL_PARALLEL_RET_SUCCESS) {
            return result;
        }
        for (uint32_t index = first; index < end; ++index) {
            if (self.cancelled()) {
                return JXL_PARALLEL_RET_RUNNER_ERROR;
            }
            function(work, index, 0);
        }
        return JXL_PARALLEL_RET_SUCCESS;
    }
    struct ParallelCall {
        JxlHandler& handler;
        void* work;
        JxlParallelRunInit init;
        JxlParallelRunFunction function;
        std::atomic_bool cancelled = false;
    };
    static JxlParallelRetCode parallelInit(void* opaque, size_t threads)
    {
        auto& call = *static_cast<ParallelCall*>(opaque);
        return call.init(call.work, threads);
    }
    static void parallelWork(void* opaque, uint32_t index, size_t thread)
    {
        auto& call = *static_cast<ParallelCall*>(opaque);
        if (call.handler.cancelled()) {
            call.cancelled.store(true, std::memory_order_relaxed);
            return;
        }
        call.function(call.work, index, thread);
    }
    static JxlParallelRetCode runParallel(void* opaque, void* work, JxlParallelRunInit init,
                                          JxlParallelRunFunction function, uint32_t first,
                                          uint32_t end)
    {
        auto& self = *static_cast<JxlHandler*>(opaque);
        if (self.cancelled()) {
            return JXL_PARALLEL_RET_RUNNER_ERROR;
        }
        ParallelCall call{self, work, init, function};
        const auto result = JxlThreadParallelRunner(self.parallelRunner_.get(), &call, parallelInit,
                                                    parallelWork, first, end);
        return call.cancelled.load(std::memory_order_relaxed) ? JXL_PARALLEL_RET_RUNNER_ERROR
                                                              : result;
    }
    bool initialize(bool metadata, bool preview = false)
    {
        decoder_.reset();
        parallelRunner_.reset();
        input_.clear();
        inputSet_ = false;
        inputClosed_ = false;
        bytesRead_ = 0;
        probing_ = metadata;
        previewDecode_ = preview;
        if (!device() || device()->isSequential() || startPosition_ < 0 ||
            !device()->seek(startPosition_) || cancelled()) {
            return fail("JPEG XL input is unavailable.");
        }
        JxlMemoryManager memory{this, allocate, release};
        decoder_.reset(JxlDecoderCreate(&memory));
        // Startup and tiny images stay serial. A large still can use every
        // logical CPU inside the single admitted decode operation.
        if (!metadata && !info_.have_animation &&
            quint64(size_.width()) * size_.height() >= 4'000'000) {
            const int available = std::max(1, QThread::idealThreadCount());
            bool validOverride = false;
            const int override =
                qEnvironmentVariableIntValue("LICASA_JXL_DECODE_WORKERS", &validOverride);
            const int workers = validOverride ? std::clamp(override, 1, available) : available;
            if (!validOverride || override != 0) {
                parallelRunner_.reset(JxlThreadParallelRunnerCreate(nullptr, size_t(workers)));
            }
        }
        const int events =
            JXL_DEC_BASIC_INFO |
            (metadata ? 0
                      : JXL_DEC_COLOR_ENCODING |
                            (preview ? JXL_DEC_PREVIEW_IMAGE : JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
        if (!decoder_ || JxlDecoderSubscribeEvents(decoder_.get(), events) != JXL_DEC_SUCCESS ||
            JxlDecoderSetParallelRunner(decoder_.get(), parallelRunner_ ? runParallel : run,
                                        this) != JXL_DEC_SUCCESS ||
            JxlDecoderSetKeepOrientation(decoder_.get(), metadata) != JXL_DEC_SUCCESS ||
            JxlDecoderSetUnpremultiplyAlpha(decoder_.get(), JXL_TRUE) != JXL_DEC_SUCCESS ||
            JxlDecoderSetCms(decoder_.get(), *JxlGetDefaultCms()) != JXL_DEC_SUCCESS ||
            JxlDecoderSetDesiredIntensityTarget(decoder_.get(), 255) != JXL_DEC_SUCCESS) {
            return fail("JPEG XL decoder initialization failed.");
        }
        if (!metadata && pendingFrame_ > 0) {
            JxlDecoderSkipFrames(decoder_.get(), size_t(pendingFrame_));
        }
        return true;
    }
    JxlDecoderStatus process()
    {
        while (!cancelled()) {
            const auto status = JxlDecoderProcessInput(decoder_.get());
            if (status != JXL_DEC_NEED_MORE_INPUT) {
                return status;
            }
            if (inputClosed_) {
                return JXL_DEC_ERROR;
            }
            const auto remaining = inputSet_ ? JxlDecoderReleaseInput(decoder_.get()) : 0;
            if (remaining > size_t(input_.size())) {
                return JXL_DEC_ERROR;
            }
            input_.remove(0, input_.size() - qsizetype(remaining));
            qsizetype chunk = inputChunk;
            if (!probing_ && !previewDecode_ && !info_.have_animation &&
                quint64(size_.width()) * size_.height() >= 4'000'000) {
                chunk = largeStillChunk;
                const qint64 sourceBytes = device()->size() - startPosition_;
                if (sourceBytes > 0 && sourceBytes <= wholeStillInputLimit &&
                    quint64(sourceBytes) <= pixelBudget_ * 4) {
                    // Supplying the bounded compressed still at once avoids
                    // repeated reconstruction at thousands of 64 KiB input
                    // boundaries in libjxl.
                    chunk = qsizetype(sourceBytes);
                }
            }
            const qsizetype limit = std::max(inputLimit, chunk * 2);
            if (input_.size() > limit - chunk || (probing_ && bytesRead_ >= metadataLimit)) {
                return JXL_DEC_ERROR;
            }
            QByteArray block = device()->read(chunk);
            if (block.isEmpty() && !device()->atEnd()) {
                return JXL_DEC_ERROR;
            }
            bytesRead_ += size_t(block.size());
            if (input_.isEmpty()) {
                input_ = std::move(block);
            } else {
                input_.append(block);
            }
            if (JxlDecoderSetInput(decoder_.get(),
                                   reinterpret_cast<const uint8_t*>(input_.constData()),
                                   size_t(input_.size())) != JXL_DEC_SUCCESS) {
                return JXL_DEC_ERROR;
            }
            inputSet_ = true;
            if (device()->atEnd()) {
                JxlDecoderCloseInput(decoder_.get());
                inputClosed_ = true;
            }
        }
        return JXL_DEC_ERROR;
    }
    bool readMetadata()
    {
        if (failed_) {
            return false;
        }
        if (metadataRead_) {
            return true;
        }
        if (!canRead()) {
            return fail("JPEG XL signature is invalid.");
        }
        pixelBudget_ = Contract::pixelBudget(device());
        allocationLimit_ = metadataLimit;
        startPosition_ = device()->pos();
        if (!initialize(true) || process() != JXL_DEC_BASIC_INFO ||
            JxlDecoderGetBasicInfo(decoder_.get(), &info_) != JXL_DEC_SUCCESS) {
            return fail("JPEG XL header is invalid or exceeds the metadata limit.");
        }
        if (!info_.xsize || !info_.ysize ||
            info_.xsize > quint32(std::numeric_limits<int>::max()) ||
            info_.ysize > quint32(std::numeric_limits<int>::max()) ||
            (info_.have_animation &&
             (!info_.animation.tps_numerator || !info_.animation.tps_denominator)) ||
            info_.preview.xsize > quint32(std::numeric_limits<int>::max()) ||
            info_.preview.ysize > quint32(std::numeric_limits<int>::max())) {
            return fail("JPEG XL dimensions or timing are invalid.");
        }
        size_ = QSize(int(info_.xsize), int(info_.ysize));
        previewSize_ = QSize(int(info_.preview.xsize), int(info_.preview.ysize));
        if (info_.orientation >= JXL_ORIENT_TRANSPOSE) {
            size_.transpose();
            previewSize_.transpose();
        }
        if (info_.have_preview && previewSize_.isValid()) {
            device()->setProperty(Contract::embeddedPreviewSizeProperty, previewSize_);
        }
        decoder_.reset();
        input_.clear();
        metadataRead_ = true;
        return true;
    }

    // Keep accounting and input alive until the native decoder is destroyed.
    std::atomic_size_t liveAllocation_ = 0;
    std::atomic_size_t peakAllocation_ = 0;
    size_t allocationLimit_ = metadataLimit;
    size_t bytesRead_ = 0;
    std::atomic_bool allocationRejected_ = false;
    bool metadataRead_ = false;
    bool failed_ = false;
    bool lastFrame_ = false;
    bool probing_ = false;
    bool previewDecode_ = false;
    bool inputSet_ = false;
    bool inputClosed_ = false;
    qint64 startPosition_ = -1;
    quint64 pixelBudget_ = 0;
    int frameNumber_ = -1;
    int pendingFrame_ = 0;
    int knownFrameCount_ = 0;
    int delayMs_ = 0;
    QSize size_;
    QSize previewSize_;
    QSize scaledSize_;
    JxlBasicInfo info_{};
    QString previewPath_ = QStringLiteral("not decoded");
    QByteArray input_;
    ParallelRunner parallelRunner_{nullptr, JxlThreadParallelRunnerDestroy};
    Decoder decoder_{nullptr, JxlDecoderDestroy};
};
} // namespace

extern "C" Q_DECL_EXPORT QImageIOHandler* licasaCreateImageHandler() { return new JxlHandler; }
