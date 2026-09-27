#include "media/preferred_cover_frame_coordinator.h"

#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using namespace Licasa;

class PreferredCoverFrameCoordinatorTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanup();

    void persistedAnimationChoiceAppliesOnlyWhenReady();
    void currentMotionPositionCanBecomePreferred();
    void currentPreferredChoiceCanBeCleared();
    void kindMismatchDoesNotApplyPersistedChoice();

  private:
    QUrl makeSource(const QString& name, const QByteArray& contents);

    QTemporaryDir settingsDir_;
    QTemporaryDir sourcesDir_;
};

void PreferredCoverFrameCoordinatorTest::initTestCase()
{
    QVERIFY(settingsDir_.isValid());
    QVERIFY(sourcesDir_.isValid());

    QCoreApplication::setOrganizationName(QStringLiteral("LicasaCoordinatorTest"));
    QCoreApplication::setApplicationName(QStringLiteral("PreferredCoverFrameCoordinator"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
    QSettings().clear();
}

void PreferredCoverFrameCoordinatorTest::cleanup()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

QUrl PreferredCoverFrameCoordinatorTest::makeSource(const QString& name, const QByteArray& contents)
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

void PreferredCoverFrameCoordinatorTest::persistedAnimationChoiceAppliesOnlyWhenReady()
{
    const QUrl source = makeSource(QStringLiteral("animation.webp"), QByteArray("animated"));
    QVERIFY(source.isValid());

    PreferredCoverFrameStore store;
    QVERIFY(store.storeAnimationFrameIndex(source, 7));

    PreferredCoverFrameCoordinator coordinator;
    qint64 applied = -1;
    int applyCalls = 0;
    coordinator.setApplyHandler([&](qint64 value) {
        ++applyCalls;
        applied = value;
        return true;
    });
    coordinator.setCurrentState(0, true, false);
    coordinator.setSource(source, PreferredCoverFrameKind::AnimationFrameIndex);

    QVERIFY(coordinator.hasPreferred());
    QCOMPARE(applyCalls, 0);

    coordinator.setCurrentState(0, true, true);
    QCOMPARE(applyCalls, 1);
    QCOMPARE(applied, qint64(7));

    coordinator.setCurrentState(7, true, true);
    QVERIFY(coordinator.currentIsPreferred());
    QCOMPARE(applyCalls, 1);
}

void PreferredCoverFrameCoordinatorTest::currentMotionPositionCanBecomePreferred()
{
    const QUrl source = makeSource(QStringLiteral("motion.jpg"), QByteArray("motion"));
    QVERIFY(source.isValid());

    PreferredCoverFrameCoordinator coordinator;
    coordinator.setCurrentState(2'345'000, true, false);
    coordinator.setSource(source, PreferredCoverFrameKind::MotionTimestampUs);
    QVERIFY(!coordinator.hasPreferred());

    coordinator.makeCurrentPreferred();
    QVERIFY(coordinator.hasPreferred());
    QVERIFY(coordinator.currentIsPreferred());

    PreferredCoverFrameStore reopened;
    const auto stored = reopened.load(source);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->kind, PreferredCoverFrameKind::MotionTimestampUs);
    QCOMPARE(stored->value, qint64(2'345'000));
}

void PreferredCoverFrameCoordinatorTest::currentPreferredChoiceCanBeCleared()
{
    const QUrl source = makeSource(QStringLiteral("clear.gif"), QByteArray("gif"));
    QVERIFY(source.isValid());

    PreferredCoverFrameCoordinator coordinator;
    coordinator.setCurrentState(3, true, true);
    coordinator.setSource(source, PreferredCoverFrameKind::AnimationFrameIndex);
    coordinator.makeCurrentPreferred();
    QVERIFY(coordinator.hasPreferred());

    coordinator.clearPreferred();
    QVERIFY(!coordinator.hasPreferred());
    QVERIFY(!coordinator.currentIsPreferred());

    PreferredCoverFrameStore reopened;
    QVERIFY(!reopened.load(source).has_value());
}

void PreferredCoverFrameCoordinatorTest::kindMismatchDoesNotApplyPersistedChoice()
{
    const QUrl source = makeSource(QStringLiteral("kind.jpg"), QByteArray("kind"));
    QVERIFY(source.isValid());

    PreferredCoverFrameStore store;
    QVERIFY(store.storeAnimationFrameIndex(source, 4));

    PreferredCoverFrameCoordinator coordinator;
    int applyCalls = 0;
    coordinator.setApplyHandler([&](qint64) {
        ++applyCalls;
        return true;
    });
    coordinator.setCurrentState(0, true, true);
    coordinator.setSource(source, PreferredCoverFrameKind::MotionTimestampUs);

    QVERIFY(!coordinator.hasPreferred());
    QCOMPARE(applyCalls, 0);
}

QTEST_GUILESS_MAIN(PreferredCoverFrameCoordinatorTest)

#include "tst_preferred_cover_frame_coordinator.moc"
