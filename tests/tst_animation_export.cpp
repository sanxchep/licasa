#include "app/application_settings.h"
#include "export/animation_export.h"
#include "imaging/codec_plugins.h"
#include "imaging/image_resource_policy.h"

#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace {
QUrl animatedGif()
{
    return QUrl::fromLocalFile(QStringLiteral(
        LICASA_SOURCE_DIR "/tests/test-assets/animated/gif/standard-transparent-320x240.gif"));
}

QUrl staticJpeg()
{
    return QUrl::fromLocalFile(QStringLiteral(
        LICASA_SOURCE_DIR "/tests/test-assets/formats/core-matrix/jpeg/full-hd-1920x1080.jpg"));
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}
} // namespace

class AnimationExportTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase()
    {
        QVERIFY(settings_.isValid());
        QCoreApplication::setOrganizationName("LicasaTests");
        QCoreApplication::setApplicationName("AnimationExportTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        preferences_ = std::make_unique<Licasa::ViewerPreferences>();
        policy_ = std::make_unique<Licasa::ImageResourcePolicy>(*preferences_);
    }

    void animatedCopyIsByteIdentical()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString destination = directory.filePath("copy.gif");
        const auto result =
            Licasa::copyAnimationFile(*policy_, animatedGif(), QUrl::fromLocalFile(destination));
        QVERIFY2(result.errorMessage.isEmpty(), qPrintable(result.errorMessage));
        QCOMPARE(result.destinationUrl, QUrl::fromLocalFile(destination));
        QCOMPARE(readAll(destination), readAll(animatedGif().toLocalFile()));
    }

    void staticImageIsRejectedWithoutDestinationDamage()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString destination = directory.filePath("copy.jpg");
        QFile existing(destination);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("keep-me"), qint64(7));
        existing.close();

        const auto result =
            Licasa::copyAnimationFile(*policy_, staticJpeg(), QUrl::fromLocalFile(destination));
        QVERIFY(!result.errorMessage.isEmpty());
        QCOMPARE(readAll(destination), QByteArray("keep-me"));
    }

    void destinationMustKeepOriginalSuffix()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString destination = directory.filePath("copy.webp");
        const auto result =
            Licasa::copyAnimationFile(*policy_, animatedGif(), QUrl::fromLocalFile(destination));
        QVERIFY(!result.errorMessage.isEmpty());
        QVERIFY(!QFileInfo::exists(destination));
    }

    void destinationCannotAliasSource()
    {
        const auto result = Licasa::copyAnimationFile(*policy_, animatedGif(), animatedGif());
        QVERIFY(!result.errorMessage.isEmpty());
    }

    void asyncServiceCompletesWithoutReplacingSource()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString destination = directory.filePath("async.gif");
        const QByteArray original = readAll(animatedGif().toLocalFile());

        Licasa::AnimationExportService service(*policy_);
        QSignalSpy completed(&service, &Licasa::AnimationExportService::exportCompleted);
        QSignalSpy failed(&service, &Licasa::AnimationExportService::exportFailed);
        QVERIFY(service.exportCopy(animatedGif(), QUrl::fromLocalFile(destination)));
        QVERIFY(service.busy());
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!service.busy());
        QCOMPARE(readAll(destination), original);
        QCOMPARE(readAll(animatedGif().toLocalFile()), original);
    }

  private:
    QTemporaryDir settings_;
    std::unique_ptr<Licasa::ViewerPreferences> preferences_;
    std::unique_ptr<Licasa::ImageResourcePolicy> policy_;
};

int main(int argc, char** argv)
{
    Licasa::ImageResourcePolicy::initializeDecoderEnvironment();
    QGuiApplication application(argc, argv);
    Licasa::initializeImagePlugins();
    AnimationExportTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_animation_export.moc"
