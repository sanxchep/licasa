#include "diagnostics_common.h"
#include "platform/application_identity.h"

#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cmath>

namespace LicasaDiagnostics {
namespace {
class HdrNavigationCheck final : public QObject, public DiagnosticCheck {
  public:
    HdrNavigationCheck(const QUrl& fixture, quint32 seed) : fixture_(fixture), random_(seed)
    {
        report_["seed"] = qint64(seed);
    }

    bool attach(QQuickWindow* window) override
    {
        window_ = window;
        report_["build"] = Licasa::applicationIdentity().toJson();
        report_["fixture_sha256"] = fileSha256(fixture_.toLocalFile());
        report_["window_id"] = windowId_;
        viewport_ = window->findChild<QQuickItem*>(QStringLiteral("imageViewport"));
        const QImage base(fixture_.toLocalFile());
        if (!viewport_ || !scratch_.isValid() || base.isNull()) {
            return false;
        }
        QFile file(fixture_.toLocalFile());
        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }
        QByteArray brokenMap = file.readAll();
        // The lab fixture ends at the gain JPEG's EOI. Break that endpoint
        // while leaving the primary JPEG and its MPF declaration intact.
        if (!brokenMap.contains(QByteArray("MPF\0", 4)) || !brokenMap.endsWith("\xff\xd9")) {
            return false;
        }
        brokenMap.back() = '\0';
        const QString brokenPath = scratch_.filePath("broken-map.jpg");
        const QString ordinaryPath = scratch_.filePath("ordinary.png");
        const QString replaceablePath = scratch_.filePath("replaceable.ppm");
        QImage ordinary(320, 240, QImage::Format_RGB32);
        ordinary.fill(QColor(35, 185, 93));
        if (!writeBytes(brokenPath, brokenMap) || QImage(brokenPath) != base ||
            !ordinary.save(ordinaryPath)) {
            return false;
        }
        const QColor baseCenter = base.pixelColor(base.width() / 2, base.height() / 2);
        images_ = {{{fixture_, baseCenter},
                    {QUrl::fromLocalFile(brokenPath), baseCenter},
                    {QUrl::fromLocalFile(ordinaryPath), ordinary.pixelColor(0, 0)},
                    {QUrl::fromLocalFile(replaceablePath), QColor()}}};
        if (!replacePixels()) {
            return false;
        }
        QTimer::singleShot(12000, this,
                           [this] { finish("Timed out waiting for the expected pixels"); });
        checkpoint(GainMap);
        return true;
    }

  private:
    enum Image { GainMap, BrokenMap, Ordinary, Replaceable, ImageCount };
    struct Sample {
        QUrl url;
        QColor center;
    };

    static bool writeBytes(const QString& path, const QByteArray& bytes)
    {
        QSaveFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
               file.commit();
    }

    bool replacePixels()
    {
        auto& sample = images_[Replaceable];
        const QColor color =
            sample.center == QColor(207, 28, 69) ? QColor(43, 61, 219) : QColor(207, 28, 69);
        const QString path = sample.url.toLocalFile();
        QFile previous(path);
        const bool existed = previous.open(QIODevice::ReadOnly);
        const auto fixedMtime = QDateTime::fromMSecsSinceEpoch(1700000000000LL);
        if (existed && previous.fileTime(QFileDevice::FileModificationTime) != fixedMtime) {
            return false;
        }
        QSaveFile output(path);
        QImage image(1600, 1200, QImage::Format_RGB888);
        image.fill(color);
        if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "ppm") || !output.commit()) {
            return false;
        }
        QFile replacement(path);
        if (!replacement.open(QIODevice::ReadWrite) ||
            (existed && replacement.size() != previous.size()) ||
            !replacement.setFileTime(fixedMtime, QFileDevice::FileModificationTime)) {
            return false;
        }
        sample.center = color;
        return true;
    }

    bool open(Image image)
    {
        const QUrl& url = images_[image].url;
        actions_.append(QJsonObject{{"action", "open"}, {"image", url.fileName()}});
        return QMetaObject::invokeMethod(window_, "loadImageUrl", Qt::DirectConnection,
                                         Q_ARG(QVariant, QVariant(url)),
                                         Q_ARG(QVariant, QVariant(true)));
    }

    void churn()
    {
        if (!window_ || !viewport_) {
            finish("Viewer disappeared during navigation");
            return;
        }
        if (step_ == 3) {
            if (!open(Replaceable)) {
                finish("Could not request the replaceable source");
                return;
            }
            // Replace it before returning to the event loop, while any
            // result queued by the outgoing request is still undelivered.
            const bool pending = viewport_->property("decodePending").toBool();
            const QString priorHash = fileSha256(images_[Replaceable].url.toLocalFile());
            if (!replacePixels()) {
                finish("Could not replace the source");
                return;
            }
            actions_.append(QJsonObject{{"action", "replaced_same_size_and_mtime"},
                                        {"decode_pending", pending},
                                        {"before_sha256", priorHash},
                                        {"after_sha256", fileSha256(images_[Replaceable].url.toLocalFile())}});
            if (!open(Replaceable)) {
                finish("Could not reopen the replaced source");
                return;
            }
        } else if (step_ == 12) {
            const auto target = checkpoints_[burst_];
            if (!open(target)) {
                finish("Could not open the checkpoint image");
                return;
            }
            checkpoint(target);
            return;
        } else if (!open(Image(random_.bounded(int(ImageCount))))) {
            finish("Could not open a burst image");
            return;
        }
        ++step_;
        const int delay = random_.bounded(13);
        actions_.append(QJsonObject{{"action", "wait"}, {"ms", delay}});
        QTimer::singleShot(delay, this, &HdrNavigationCheck::churn);
    }

    void checkpoint(Image image, int matchingFrames = 0)
    {
        if (!window_ || !viewport_) {
            finish("Viewer disappeared before the checkpoint");
            return;
        }
        const auto& expected = images_[image];
        const QString provider = window_->property("renderedProviderSource").toString();
        const bool ready =
            viewport_->property("imageStatus").toInt() == 1 &&
            !viewport_->property("decodePending").toBool() &&
            viewport_->property("fadingImageSlot").toInt() == -1 &&
            window_->property("residentImageUrl").toUrl() == expected.url && !provider.isEmpty() &&
            viewport_->property("activeRequestKey").toString().startsWith(provider + '&');
        if (ready) {
            const QImage frame = window_->grabWindow();
            if (frame.width() < 6 || frame.height() < 6) {
                finish("The settled viewer did not produce a usable frame");
                return;
            }
            QJsonArray samples;
            for (int offset : {-2, 0, 2}) {
                const QColor pixel = frame.pixelColor(frame.width() / 2 + offset, frame.height() / 2);
                report_["last_pixel"] = pixel.name();
                report_["expected_pixel"] = expected.center.name();
                if (!pixel.isValid() ||
                    std::max({std::abs(pixel.red() - expected.center.red()),
                              std::abs(pixel.green() - expected.center.green()),
                              std::abs(pixel.blue() - expected.center.blue())}) > 3) {
                    finish("The settled viewer rendered the wrong pixels");
                    return;
                }
                samples.append(pixel.name());
            }
            sampledFrames_.append(samples);
            if (++matchingFrames == 3) {
                actions_.append(QJsonObject{{"action", "verified"},
                                            {"image", expected.url.fileName()},
                                            {"window_id", windowId_},
                                            {"request", viewport_->property("activeRequestKey").toString()},
                                            {"source_sha256", fileSha256(expected.url.toLocalFile())},
                                            {"frames", sampledFrames_}});
                sampledFrames_ = {};
                if (++burst_ == int(checkpoints_.size())) {
                    finish(nullptr);
                } else {
                    step_ = 0;
                    churn();
                }
                return;
            }
        } else if (matchingFrames) {
            finish("A settled image lost its current request during the checkpoint");
            return;
        }
        QTimer::singleShot(50, this,
                           [this, image, matchingFrames] { checkpoint(image, matchingFrames); });
    }

    void finish(const char* error)
    {
        report_["actions"] = actions_;
        if (viewport_) {
            report_["active_request"] = viewport_->property("activeRequestKey").toString();
            report_["pending_request"] = viewport_->property("pendingRequestKey").toString();
        }
        report_["status"] = error ? "failed" : "passed";
        if (error) {
            report_["error"] = error;
        }
        printJson(report_);
        if (window_) {
            window_->close();
        }
        QCoreApplication::exit(error ? 1 : 0);
    }

    const QUrl fixture_;
    QRandomGenerator random_;
    QTemporaryDir scratch_;
    QPointer<QQuickWindow> window_;
    QPointer<QQuickItem> viewport_;
    std::array<Sample, ImageCount> images_;
    const std::array<Image, 4> checkpoints_{{BrokenMap, Replaceable, Ordinary, GainMap}};
    int burst_ = -1;
    int step_ = 0;
    QJsonObject report_;
    QJsonArray actions_;
    QJsonArray sampledFrames_;
    const QString windowId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
};
} // namespace

std::unique_ptr<DiagnosticCheck> makeHdrNavigationCheck(const QUrl& fixture, quint32 seed)
{
    return std::make_unique<HdrNavigationCheck>(fixture, seed);
}
} // namespace LicasaDiagnostics
