#include "diagnostics_common.h"
#include "diagnostics_playback_click.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQuickWindow>
#include <QTimer>
#include <QtTest>

namespace {
class ControlledViewport final : public QObject {
    Q_OBJECT
  public:
    explicit ControlledViewport(QObject* parent) : QObject(parent)
    {
        setObjectName(QStringLiteral("imageViewport"));
        setProperty("imageStatus", 1);
        setProperty("pendingImageSlot", -1);
    }

  signals:
    void imageReady();
    void playbackToggleRequested();
};

class ControlledWindow final : public QQuickWindow {
    Q_OBJECT
  public:
    ControlledWindow() : viewport(this)
    {
        setProperty("previewPhase", true);
        setProperty("rawFastPhase", false);
        setProperty("rawFullDetailRequested", false);
        setProperty("imageLimitedToPreview", false);
        setProperty("naturalImageSize", QSize(40, 30));
    }

    Q_INVOKABLE void clearImageState() { viewport.setProperty("imageStatus", 0); }

    Q_INVOKABLE void loadImageUrl(const QVariant& url, const QVariant&)
    {
        setProperty("residentImageUrl", url);
        viewport.setProperty("imageStatus", 1);
        emit viewport.imageReady();
        QTimer::singleShot(0, this, [this] { emit frameSwapped(); });
    }

    ControlledViewport viewport;
};

int runControlledDriver(QGuiApplication& application, const QString& driver, const QString& mode)
{
    ControlledWindow window;
    QJsonObject controlledEvents;
    std::unique_ptr<LicasaDiagnostics::DiagnosticCheck> check;
    if (driver == QStringLiteral("raw-policy")) {
        const bool deferred = mode.startsWith(QStringLiteral("deferred-"));
        const bool forbidden = mode.endsWith(QStringLiteral("forbidden"));
        window.setProperty("imageLimitedToPreview", !deferred);
        window.viewport.setProperty(
            "activeRequestKey", QStringLiteral("image://controlled/photo?licasa_stage=preview"));
        check = LicasaDiagnostics::makeRawBehaviorCheck(false, false);
        if (deferred) {
            QTimer::singleShot(40, &window,
                               [&window] { window.setProperty("imageLimitedToPreview", true); });
        }
        if (forbidden) {
            QTimer::singleShot(deferred ? 140 : 100, &window, [&window] {
                window.setProperty("rawFullDetailRequested", true);
                window.setProperty("previewPhase", false);
            });
        }
    } else if (driver == QStringLiteral("viewer-ready")) {
        const QUrl source(QStringLiteral("file:///controlled/photo.png"));
        window.setProperty("residentImageUrl", source);
        window.setProperty("previewPhase", false);
        window.setProperty("rawFastPhase", true);
        window.viewport.setProperty(
            "activeRequestKey", QStringLiteral("image://controlled/photo?licasa_stage=raw-fast"));
        const qint64 started = LicasaDiagnostics::monotonicNs();
        check = LicasaDiagnostics::makeViewerMeasurement(started, source, 1);
        QTimer::singleShot(20, &window, [&window, &controlledEvents, started] {
            controlledEvents["raw_fast_ready_ms"] =
                (LicasaDiagnostics::monotonicNs() - started) / 1e6;
            emit window.viewport.imageReady();
            emit window.frameSwapped();
        });
        QTimer::singleShot(60, &window, [&window, &controlledEvents, started] {
            window.setProperty("rawFastPhase", false);
            window.viewport.setProperty(
                "activeRequestKey",
                QStringLiteral("image://controlled/photo?licasa_stage=preview"));
            controlledEvents["staged_preview_ready_ms"] =
                (LicasaDiagnostics::monotonicNs() - started) / 1e6;
            emit window.viewport.imageReady();
            emit window.frameSwapped();
        });
        QTimer::singleShot(120, &window, [&window, &controlledEvents, started, mode] {
            const QString source =
                mode == QStringLiteral("unstaged")
                    ? QStringLiteral("image://controlled/animation-frame")
                    : QStringLiteral("image://controlled/photo?licasa_stage=") + mode;
            window.viewport.setProperty("activeRequestKey", source);
            controlledEvents["final_raster_ready_ms"] =
                (LicasaDiagnostics::monotonicNs() - started) / 1e6;
            emit window.viewport.imageReady();
            emit window.frameSwapped();
        });
    } else {
        return 3;
    }
    if (!check->attach(&window)) {
        return 4;
    }
    const int result = application.exec();
    if (!controlledEvents.isEmpty()) {
        LicasaDiagnostics::printJson(QJsonObject{{"controlled_events", controlledEvents}});
    }
    return result;
}

struct ChildResult {
    bool finished = false;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    int exitCode = -1;
    QByteArray output;
    QByteArray errors;
};

ChildResult runChild(const QString& driver, const QString& mode)
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
    environment.insert(QStringLiteral("QSG_RENDER_LOOP"), QStringLiteral("basic"));
    process.setProcessEnvironment(environment);
    process.start(QCoreApplication::applicationFilePath(),
                  {QStringLiteral("--driver-child"), driver, mode});
    ChildResult result;
    result.finished = process.waitForFinished(10000);
    if (!result.finished) {
        process.kill();
        process.waitForFinished(1000);
    }
    result.exitStatus = process.exitStatus();
    result.exitCode = process.exitCode();
    result.output = process.readAllStandardOutput();
    result.errors = process.readAllStandardError();
    return result;
}
} // namespace

class DiagnosticDriverTests final : public QObject {
    Q_OBJECT
  private slots:
    void delayedPlaybackClick_data()
    {
        QTest::addColumn<bool>("expectedPlaying");
        QTest::newRow("pause") << false;
        QTest::newRow("resume") << true;
    }

    void delayedPlaybackClick()
    {
        QFETCH(bool, expectedPlaying);
        using Check = LicasaDiagnostics::PlaybackClickCheck;
        ControlledViewport viewport(nullptr);
        Check check(&viewport, expectedPlaying);
        bool playing = !expectedPlaying;
        // Reproduce a busy GUI thread: the observer can run before a delayed
        // QML click timer even after the old double-click interval + 50 ms.
        QTest::qSleep(650);
        QCOMPARE(check.check(true, playing), Check::Outcome::Pending);
        QTimer::singleShot(0, &viewport, [&] { emit viewport.playbackToggleRequested(); });
        QTRY_COMPARE(check.requests(), 1);
        QCOMPARE(check.check(true, playing), Check::Outcome::Pending);
        // The media backend need not apply the intent in the same event turn.
        QTimer::singleShot(40, &viewport, [&] { playing = expectedPlaying; });
        QTRY_COMPARE(check.check(true, playing), Check::Outcome::Applied);
    }

    void matchingPlaybackStateRequiresClickIntent()
    {
        using Check = LicasaDiagnostics::PlaybackClickCheck;
        ControlledViewport viewport(nullptr);
        Check check(&viewport, false, 80);
        QCOMPARE(check.check(true, false), Check::Outcome::Pending);
        QCOMPARE(check.check(false, false), Check::Outcome::Pending);
        QTest::qSleep(100);
        QCOMPARE(check.check(true, false), Check::Outcome::Failed);
    }

    void playbackClickRequiresActiveSessionAndAppliedState()
    {
        using Check = LicasaDiagnostics::PlaybackClickCheck;
        ControlledViewport viewport(nullptr);
        Check check(&viewport, false, 80);
        emit viewport.playbackToggleRequested();
        QCOMPARE(check.check(false, false), Check::Outcome::Pending);
        QCOMPARE(check.check(true, true), Check::Outcome::Pending);
        QTest::qSleep(100);
        QCOMPARE(check.check(true, true), Check::Outcome::Failed);
    }

    void duplicatePlaybackClickFails()
    {
        using Check = LicasaDiagnostics::PlaybackClickCheck;
        ControlledViewport viewport(nullptr);
        Check check(&viewport, false);
        emit viewport.playbackToggleRequested();
        emit viewport.playbackToggleRequested();
        QCOMPARE(check.requests(), 2);
        QCOMPARE(check.check(true, false), Check::Outcome::Failed);
    }

    void playbackClickObservationIsScopedToItsGesture()
    {
        using Check = LicasaDiagnostics::PlaybackClickCheck;
        ControlledViewport viewport(nullptr);
        {
            Check pause(&viewport, false);
            emit viewport.playbackToggleRequested();
            QCOMPARE(pause.check(true, false), Check::Outcome::Applied);
        }
        Check resume(&viewport, true);
        QCOMPARE(resume.requests(), 0);
        QCOMPARE(resume.check(true, true), Check::Outcome::Pending);
        emit viewport.playbackToggleRequested();
        QCOMPARE(resume.check(true, true), Check::Outcome::Applied);
    }

    void playbackClickRequiresConnectedViewport()
    {
        using Check = LicasaDiagnostics::PlaybackClickCheck;
        Check missing(nullptr, false);
        QCOMPARE(missing.check(true, false), Check::Outcome::Failed);
    }

    void rawPolicyObservation_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("forbidden");
        QTest::newRow("immediate-allowed") << QStringLiteral("allowed") << false;
        QTest::newRow("immediate-forbidden") << QStringLiteral("forbidden") << true;
        QTest::newRow("deferred-allowed") << QStringLiteral("deferred-allowed") << false;
        QTest::newRow("deferred-forbidden") << QStringLiteral("deferred-forbidden") << true;
    }

    void rawPolicyObservation()
    {
        QFETCH(QString, mode);
        QFETCH(bool, forbidden);
        const ChildResult child = runChild(QStringLiteral("raw-policy"), mode);
        QVERIFY2(child.finished, child.errors.constData());
        QCOMPARE(child.exitStatus, QProcess::NormalExit);
        QCOMPARE(child.exitCode, forbidden ? 2 : 0);
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(child.output, &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        QVERIFY(document.isObject());
        const QJsonObject report = document.object();
        if (forbidden) {
            QVERIFY(
                report.value("error").toString().contains(QStringLiteral("Policy-limited RAW")));
            QVERIFY(!report.value("full_detail_blocked_by_policy").toBool());
        } else {
            QVERIFY(!report.contains("error"));
            QVERIFY(report.value("full_detail_blocked_by_policy").toBool());
            QVERIFY(report.value("policy_observation_ms").toDouble() >= 250.0);
        }
    }

    void viewerReadyWaitsForFinalRaster_data()
    {
        QTest::addColumn<QString>("stage");
        QTest::newRow("ordinary-full") << QStringLiteral("full");
        QTest::newRow("raw-interactive") << QStringLiteral("raw-interactive");
        QTest::newRow("unstaged-animation") << QStringLiteral("unstaged");
    }

    void viewerReadyWaitsForFinalRaster()
    {
        QFETCH(QString, stage);
        const ChildResult child = runChild(QStringLiteral("viewer-ready"), stage);
        QVERIFY2(child.finished, child.errors.constData());
        QCOMPARE(child.exitStatus, QProcess::NormalExit);
        QCOMPARE(child.exitCode, 0);
        QJsonParseError error;
        const QList<QByteArray> reports = child.output.trimmed().split('\n');
        QCOMPARE(reports.size(), 2);
        const QJsonDocument document = QJsonDocument::fromJson(reports.at(0), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        QVERIFY(document.isObject());
        const QJsonObject report = document.object();
        QVERIFY(!report.contains("error"));
        const QJsonDocument controlled = QJsonDocument::fromJson(reports.at(1), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        const QJsonObject events = controlled.object().value("controlled_events").toObject();
        QVERIFY(events.contains("raw_fast_ready_ms"));
        QVERIFY(events.contains("staged_preview_ready_ms"));
        QVERIFY(events.contains("final_raster_ready_ms"));
        QVERIFY(report.contains("preview_ready_ms"));
        QVERIFY(report.contains("full_detail_ready_ms"));
        QVERIFY(report.contains("full_detail_presented_ms"));
        QVERIFY(report.value("preview_ready_ms").toDouble() <=
                events.value("staged_preview_ready_ms").toDouble());
        QVERIFY(report.value("full_detail_ready_ms").toDouble() >=
                events.value("final_raster_ready_ms").toDouble());
        QVERIFY(report.value("full_detail_presented_ms").toDouble() >=
                report.value("full_detail_ready_ms").toDouble());
    }
};

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() == 4 && arguments.at(1) == QStringLiteral("--driver-child")) {
        return runControlledDriver(application, arguments.at(2), arguments.at(3));
    }
    DiagnosticDriverTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tst_diagnostic_drivers.moc"
