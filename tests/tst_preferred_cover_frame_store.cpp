#include "media/preferred_cover_frame_store.h"

#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

using namespace Licasa;

class PreferredCoverFrameStoreTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanup();

    void motionTimestampPersistsAcrossStoreInstances();
    void animationFramePersistsAcrossStoreInstances();
    void sourceMutationInvalidatesPersistedChoice();
    void malformedPersistedValueIsIgnored();
    void settingsDoNotContainRawSourcePath();
    void nonLocalSourcesAreRejected();
    void clearRemovesOnlyTheRequestedAsset();

  private:
    QUrl makeSource(const QString& name, const QByteArray& contents);

    QTemporaryDir settingsDir_;
    QTemporaryDir sourcesDir_;
};

void PreferredCoverFrameStoreTest::initTestCase()
{
    QVERIFY(settingsDir_.isValid());
    QVERIFY(sourcesDir_.isValid());

    QCoreApplication::setOrganizationName(QStringLiteral("LicasaStoreTest"));
    QCoreApplication::setApplicationName(QStringLiteral("PreferredCoverFrameStore"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
    QSettings().clear();
}

void PreferredCoverFrameStoreTest::cleanup()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

QUrl PreferredCoverFrameStoreTest::makeSource(const QString& name, const QByteArray& contents)
{
    const QString path = sourcesDir_.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {};
    }
    if (file.write(contents) != contents.size()) {
        return {};
    }
    file.close();
    return QUrl::fromLocalFile(path);
}

void PreferredCoverFrameStoreTest::motionTimestampPersistsAcrossStoreInstances()
{
    const QUrl source = makeSource(QStringLiteral("motion.jpg"), QByteArray("motion-source"));
    QVERIFY(source.isValid());

    {
        PreferredCoverFrameStore store;
        QString error;
        QVERIFY2(store.storeMotionTimestampUs(source, 1'234'567, &error), qPrintable(error));
    }

    PreferredCoverFrameStore reopened;
    QString error;
    const auto preference = reopened.load(source, &error);
    QVERIFY2(preference.has_value(), qPrintable(error));
    QCOMPARE(preference->kind, PreferredCoverFrameKind::MotionTimestampUs);
    QCOMPARE(preference->value, qint64(1'234'567));
}

void PreferredCoverFrameStoreTest::animationFramePersistsAcrossStoreInstances()
{
    const QUrl source = makeSource(QStringLiteral("animation.webp"), QByteArray("animation"));
    QVERIFY(source.isValid());

    PreferredCoverFrameStore writer;
    QString error;
    QVERIFY2(writer.storeAnimationFrameIndex(source, 42, &error), qPrintable(error));

    PreferredCoverFrameStore reader;
    const auto preference = reader.load(source, &error);
    QVERIFY2(preference.has_value(), qPrintable(error));
    QCOMPARE(preference->kind, PreferredCoverFrameKind::AnimationFrameIndex);
    QCOMPARE(preference->value, qint64(42));
}

void PreferredCoverFrameStoreTest::sourceMutationInvalidatesPersistedChoice()
{
    const QUrl source = makeSource(QStringLiteral("replace.heic"), QByteArray("before"));
    QVERIFY(source.isValid());

    PreferredCoverFrameStore store;
    QString error;
    QVERIFY2(store.storeMotionTimestampUs(source, 900'000, &error), qPrintable(error));
    QVERIFY(store.load(source).has_value());

    QFile file(source.toLocalFile());
    QVERIFY(file.open(QIODevice::Append));
    QCOMPARE(file.write(QByteArray("-changed")), qint64(8));
    file.close();

    QVERIFY(!store.load(source).has_value());
}

void PreferredCoverFrameStoreTest::malformedPersistedValueIsIgnored()
{
    const QUrl source = makeSource(QStringLiteral("malformed.gif"), QByteArray("gif"));
    QVERIFY(source.isValid());

    PreferredCoverFrameStore store;
    QVERIFY(store.storeAnimationFrameIndex(source, 7));

    QSettings settings;
    const QStringList keys = settings.allKeys();
    const auto valueIt = std::find_if(keys.cbegin(), keys.cend(), [](const QString& key) {
        return key.endsWith(QStringLiteral("/value"));
    });
    QVERIFY(valueIt != keys.cend());
    settings.setValue(*valueIt, QStringLiteral("-1"));
    settings.sync();

    QVERIFY(!store.load(source).has_value());
}

void PreferredCoverFrameStoreTest::settingsDoNotContainRawSourcePath()
{
    const QUrl source = makeSource(QStringLiteral("private-name.jpg"), QByteArray("private"));
    QVERIFY(source.isValid());

    PreferredCoverFrameStore store;
    QVERIFY(store.storeMotionTimestampUs(source, 123));

    QSettings settings;
    for (const QString& key : settings.allKeys()) {
        QVERIFY2(!key.contains(source.toLocalFile()), qPrintable(key));
        QVERIFY2(!settings.value(key).toString().contains(source.toLocalFile()), qPrintable(key));
    }
}

void PreferredCoverFrameStoreTest::nonLocalSourcesAreRejected()
{
    const QUrl remote(QStringLiteral("https://example.invalid/photo.jpg"));
    PreferredCoverFrameStore store;
    QString error;

    QVERIFY(!store.storeMotionTimestampUs(remote, 100, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!store.load(remote).has_value());
    QVERIFY(!store.clear(remote));
}

void PreferredCoverFrameStoreTest::clearRemovesOnlyTheRequestedAsset()
{
    const QUrl first = makeSource(QStringLiteral("first.jpg"), QByteArray("first"));
    const QUrl second = makeSource(QStringLiteral("second.jpg"), QByteArray("second"));
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());

    PreferredCoverFrameStore store;
    QVERIFY(store.storeMotionTimestampUs(first, 111));
    QVERIFY(store.storeAnimationFrameIndex(second, 9));

    QVERIFY(store.clear(first));
    QVERIFY(!store.load(first).has_value());

    const auto secondPreference = store.load(second);
    QVERIFY(secondPreference.has_value());
    QCOMPARE(secondPreference->kind, PreferredCoverFrameKind::AnimationFrameIndex);
    QCOMPARE(secondPreference->value, qint64(9));
}

QTEST_GUILESS_MAIN(PreferredCoverFrameStoreTest)

#include "tst_preferred_cover_frame_store.moc"
