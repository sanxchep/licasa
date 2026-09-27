#include "imaging/compute/edit_compute.h"
#include "imaging/image_edit_pipeline.h"

#include <QColorSpace>
#include <QRandomGenerator>
#include <QTest>

#include <future>

using namespace Licasa;

class GpuImageEditsTest final : public QObject {
    Q_OBJECT

  private:
    ImageEditBackend backend = ImageEditBackend::OpenCl;

    QImage fixture(QSize size)
    {
        QImage image(size, QImage::Format_ARGB32);
        QRandomGenerator random(0x9346u);
        for (int y = 0; y < size.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < size.width(); ++x) {
                row[x] = random.generate();
            }
        }
        image.setColorSpace(QColorSpace::DisplayP3);
        image.setText("caption", "untouched metadata");
        image.setDotsPerMeterX(1234);
        image.setDotsPerMeterY(4321);
        image.setDevicePixelRatio(1.5);
        image.setOffset(QPoint(7, 11));
        return image;
    }

    void compareEdits(const QImage& source, const ImageEditParameters& edit)
    {
        const QImage unchanged = source.copy();
        QImage cpu = source, gpu = source;
        ImageEditExecution reference, candidate;
        reference.requested = ImageEditBackend::Cpu;
        candidate.requested = backend;
        QVERIFY(applyImageEdits(cpu, edit, nullptr, &reference));
        QVERIFY(applyImageEdits(gpu, edit, nullptr, &candidate));
        QCOMPARE(candidate.used, backend);
        QCOMPARE(gpu, cpu);
        QCOMPARE(gpu.colorSpace(), cpu.colorSpace());
        QCOMPARE(gpu.text("caption"), cpu.text("caption"));
        QCOMPARE(gpu.dotsPerMeterX(), cpu.dotsPerMeterX());
        QCOMPARE(gpu.dotsPerMeterY(), cpu.dotsPerMeterY());
        QCOMPARE(gpu.devicePixelRatio(), cpu.devicePixelRatio());
        QCOMPARE(gpu.offset(), cpu.offset());
        QCOMPARE(source, unchanged);
    }

  private slots:
    void initTestCase()
    {
        if (qgetenv("LICASA_TEST_EDIT_BACKEND") == "cuda") {
            backend = ImageEditBackend::Cuda;
        }
        ImageEditExecution execution;
        execution.requested = backend;
        ImageEditParameters edit;
        edit.exposure = 0.4;
        QImage image = fixture(QSize(23, 19));
        QVERIFY(applyImageEdits(image, edit, nullptr, &execution));
        if (execution.used != backend) {
            if (qEnvironmentVariableIsSet("LICASA_REQUIRE_GPU_EDITS")) {
                QFAIL(qPrintable(execution.detail));
            }
            QSKIP(qPrintable(execution.detail));
        }
    }

    void explicitWarmupUsesSelectedBackend()
    {
        ImageEditExecution execution;
        execution.requested = backend;
        QVERIFY(warmGpuImageEdits(execution));
        QCOMPARE(execution.used, backend);
        QCOMPARE(retainedGpuImageEditBytes(), size_t(0));
    }

    void closedPictureReleasesDeviceRasters()
    {
        ImageEditParameters edit;
        edit.exposure = 0.4;
        const QImage source = fixture(QSize(1027, 1023));
        QImage before = source;
        ImageEditExecution first;
        first.requested = backend;
        QVERIFY(applyImageEdits(before, edit, nullptr, &first));
        QCOMPARE(first.used, backend);
        QVERIFY(retainedGpuImageEditBytes() >= 2 * size_t(source.sizeInBytes()));

        releaseGpuImageEditBuffers();
        QCOMPARE(retainedGpuImageEditBytes(), size_t(0));

        QImage after = source;
        ImageEditExecution second;
        second.requested = backend;
        QVERIFY(applyImageEdits(after, edit, nullptr, &second));
        QCOMPARE(second.used, backend);
        QCOMPARE(after, before);
    }

    void pixelsFormatsGeometryAndMetadata()
    {
        QRandomGenerator random(0x934u);
        for (const QSize size :
             {QSize(1, 1), QSize(1, 19), QSize(23, 1), QSize(37, 29), QSize(1027, 1023)}) {
            const QImage original = fixture(size);
            for (const auto format : {QImage::Format_ARGB32, QImage::Format_RGBA8888,
                                      QImage::Format_ARGB32_Premultiplied, QImage::Format_RGBA64}) {
                const QImage source = original.convertToFormat(format);
                for (int trial = 0; trial < 8; ++trial) {
                    ImageEditParameters edit;
                    edit.exposure = random.generateDouble() * 2 - 1;
                    edit.contrast = random.generateDouble() * 2 - 1;
                    edit.highlights = random.generateDouble() * 2 - 1;
                    edit.shadows = random.generateDouble() * 2 - 1;
                    edit.saturation = random.generateDouble() * 2 - 1;
                    edit.vibrance = random.generateDouble() * 2 - 1;
                    edit.warmth = random.generateDouble() * 2 - 1;
                    edit.tint = random.generateDouble() * 2 - 1;
                    edit.vignette = random.generateDouble();
                    edit.sharpen = trial % 2 ? 0.7 : 0.0;
                    edit.soften = trial % 3 ? 0.0 : 0.3;
                    edit.quarterTurns = trial % 4;
                    edit.flipHorizontal = trial % 2;
                    edit.flipVertical = trial % 3;
                    edit.cropX = 0.1;
                    edit.cropY = 0.2;
                    edit.cropWidth = 0.8;
                    edit.cropHeight = 0.7;
                    compareEdits(source, edit);
                }
            }
        }
    }

    void paddedRowsAndSharpenBorders()
    {
        QByteArray storage(400 * 19, '\x59');
        QImage padded(reinterpret_cast<uchar*>(storage.data()), 23, 19, 400, QImage::Format_ARGB32);
        ImageEditParameters edit;
        edit.sharpen = 1.0;
        compareEdits(padded, edit);
        compareEdits(fixture(QSize(3, 3)), edit);
    }

    void cancellationAndWorkerMigration()
    {
        // The cached context must work on sequential, different worker threads;
        // QThreadPool may retire the thread that first initialized it.
        const QImage source = fixture(QSize(2048, 2048));
        for (int trial = 0; trial < 3; ++trial) {
            auto work = std::async(std::launch::async, [&, trial] {
                ImageEditExecution execution;
                execution.requested = backend;
                ImageEditParameters edit;
                edit.vignette = 0.8;
                edit.sharpen = 0.7;
                QImage image = source;
                std::atomic_bool cancelled{trial == 0};
                const bool ok = applyImageEdits(image, edit, &cancelled, &execution);
                return trial == 0 ? !ok && image == source : ok && execution.used == backend;
            });
            QVERIFY(work.get());
        }
        std::atomic_bool cancelled{false};
        auto work = std::async(std::launch::async, [&] {
            QImage image = source;
            ImageEditParameters edit;
            edit.vignette = 1.0;
            ImageEditExecution execution;
            execution.requested = backend;
            return applyImageEdits(image, edit, &cancelled, &execution);
        });
        cancelled.store(true, std::memory_order_relaxed);
        QVERIFY(!work.get());
    }
};

QTEST_GUILESS_MAIN(GpuImageEditsTest)
#include "tst_gpu_image_edits.moc"
