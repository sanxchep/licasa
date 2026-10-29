#include "platform/application_identity.h"
#include "platform/lifecycle_control.h"
#include "platform/lifecycle_launch_gate.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QtTest>

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

class LifecyclePlatformTests final : public QObject {
    Q_OBJECT

  private slots:
    void init()
    {
        for (const char* key :
             {"APPIMAGE", "APPDIR", "SNAP_NAME", "SNAP_INSTANCE_NAME", "SNAP_REVISION"}) {
            environment_.insert(key, qgetenv(key));
            qunsetenv(key);
        }
    }

    void cleanup()
    {
        for (auto it = environment_.cbegin(); it != environment_.cend(); ++it) {
            if (it.value().isNull()) {
                qunsetenv(it.key().constData());
            } else {
                qputenv(it.key().constData(), it.value());
            }
        }
        environment_.clear();
    }

    void snapRevisionPreservesInstallationAndParallelInstanceDoesNotCollide()
    {
        qputenv("SNAP_NAME", "licasa");
        qputenv("SNAP_INSTANCE_NAME", "licasa");
        qputenv("SNAP_REVISION", "10");
        const auto first = Licasa::applicationIdentity();
        qputenv("SNAP_REVISION", "11");
        const auto next = Licasa::applicationIdentity();
        QCOMPARE(next.installationId, first.installationId);
        QCOMPARE(next.packageRevision, QStringLiteral("11"));
        qputenv("SNAP_INSTANCE_NAME", "licasa_review");
        QVERIFY(Licasa::applicationIdentity().installationId != first.installationId);
    }

    void portableCopiesExtractedAndFilesystemRemainDistinct()
    {
        const auto local = Licasa::applicationIdentity();
        qputenv("APPDIR", "/tmp/.mount_Licasa-random");
        qputenv("APPIMAGE", "/home/example/Licasa.AppImage");
        const auto portable = Licasa::applicationIdentity();
        qputenv("APPDIR", "/tmp/.mount_Licasa-different");
        QCOMPARE(Licasa::applicationIdentity().installationId, portable.installationId);
        qputenv("APPIMAGE", "/home/example/Licasa-copy.AppImage");
        QVERIFY(Licasa::applicationIdentity().installationId != portable.installationId);
        qunsetenv("APPIMAGE");
        const auto extracted = Licasa::applicationIdentity();
        QVERIFY(extracted.installationId != portable.installationId);
        QVERIFY(extracted.installationId != local.installationId);
        QCOMPARE(extracted.packageFamily, QStringLiteral("appimage"));
    }

    void processReceiptMatchesSelf()
    {
        const auto identity = Licasa::applicationIdentity();
        QCOMPARE(identity.generation.size(), 64);
        QCOMPARE(identity.installationId.size(), 32);
        QVERIFY(!Licasa::processStartIdentity(QCoreApplication::applicationPid()).isEmpty());
        QVERIFY(Licasa::processStartIdentity(99999999).isEmpty());
        QCOMPARE(identity.generation,
                 Licasa::processExecutableHash(QCoreApplication::applicationPid()));
    }

    void launchGateBlocksInterruptedAndConcurrentUpdates()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString root = temporary.filePath(QStringLiteral("install"));
        qputenv("APPDIR", root.toUtf8());
        const QString gate = temporary.filePath(QStringLiteral(".install.licasa-lifecycle"));
        QVERIFY(QDir().mkdir(gate));
        QVERIFY(
            QFile::setPermissions(gate, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QFile lock(gate + QStringLiteral("/lock"));
        QVERIFY(lock.open(QIODevice::ReadWrite));
        QVERIFY(lock.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
        QString reason;
        {
            Licasa::LifecycleLaunchGate launch;
            QVERIFY2(launch.acquire(&reason), qPrintable(reason));
        }
        QFile journal(gate + QStringLiteral("/transaction.json"));
        QVERIFY(journal.open(QIODevice::WriteOnly));
        QVERIFY(journal.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
        journal.write("{\"state\":\"replacing\"}");
        journal.close();
        {
            Licasa::LifecycleLaunchGate launch;
            QVERIFY(!launch.acquire(&reason));
        }
        QVERIFY(journal.open(QIODevice::WriteOnly | QIODevice::Truncate));
        journal.write("{\"state\":\"verified\"}");
        journal.close();
        {
            Licasa::LifecycleLaunchGate launch;
            QVERIFY2(launch.acquire(&reason), qPrintable(reason));
        }
        QVERIFY(::flock(lock.handle(), LOCK_EX | LOCK_NB) == 0);
        {
            Licasa::LifecycleLaunchGate launch;
            QVERIFY(!launch.acquire(&reason));
        }
        QVERIFY(::flock(lock.handle(), LOCK_UN) == 0);
        QVERIFY(journal.remove());
        QFile target(temporary.filePath(QStringLiteral("foreign")));
        QVERIFY(target.open(QIODevice::WriteOnly));
        target.write("{\"state\":\"verified\"}");
        target.close();
        QVERIFY(QFile::link(target.fileName(), journal.fileName()));
        Licasa::LifecycleLaunchGate launch;
        QVERIFY(!launch.acquire(&reason));
        QVERIFY(QFileInfo(journal).isSymLink());
    }

    void runtimeStartupLockRetainsInodeAndExcludesConcurrentOwners()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("viewer.launch"));
        QString reason;
        Licasa::RuntimeStartupLock first;
        QVERIFY2(first.acquire(path, &reason), qPrintable(reason));
        struct stat original {};
        QVERIFY(::lstat(QFile::encodeName(path).constData(), &original) == 0);
        QVERIFY(S_ISREG(original.st_mode));
        QCOMPARE(original.st_uid, ::geteuid());
        QCOMPARE(original.st_mode & 07777, mode_t(0600));
        QCOMPARE(original.st_nlink, nlink_t(1));
        QCOMPARE(original.st_size, off_t(0));
        // An old Qt client must not unlink a live permanent empty lock as stale.
        QLockFile oldAttempt(path);
        QVERIFY(!oldAttempt.tryLock(0));
        struct stat afterOldAttempt {};
        QVERIFY(::lstat(QFile::encodeName(path).constData(), &afterOldAttempt) == 0);
        QCOMPARE(afterOldAttempt.st_dev, original.st_dev);
        QCOMPARE(afterOldAttempt.st_ino, original.st_ino);
        Licasa::RuntimeStartupLock contender;
        QVERIFY(!contender.acquire(path, &reason, 0));
        QVERIFY(reason.contains(QStringLiteral("in progress")));
        first.release();
        QVERIFY(QFileInfo::exists(path));
        QVERIFY2(contender.acquire(path, &reason, 0), qPrintable(reason));
        struct stat retained {};
        QVERIFY(::lstat(QFile::encodeName(path).constData(), &retained) == 0);
        QCOMPARE(retained.st_dev, original.st_dev);
        QCOMPARE(retained.st_ino, original.st_ino);
    }

    void runtimeStartupLockCooperatesWithLegacyQtLock()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("viewer.launch"));
        QLockFile legacy(path);
        QVERIFY(legacy.tryLock(0));
        QString reason;
        Licasa::RuntimeStartupLock current;
        QVERIFY(!current.acquire(path, &reason, 20));
        legacy.unlock();
        QVERIFY(!QFileInfo::exists(path));
        QFile stale(path);
        QVERIFY(stale.open(QIODevice::WriteOnly));
        const QByteArray contents("stale legacy PID record\n");
        QCOMPARE(stale.write(contents), contents.size());
        QVERIFY(stale.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup |
                                     QFile::ReadOther));
        stale.close();
        QVERIFY2(current.acquire(path, &reason), qPrintable(reason));
        struct stat before {};
        QVERIFY(::lstat(QFile::encodeName(path).constData(), &before) == 0);
        QCOMPARE(before.st_mode & 07777, mode_t(0600));
        // Even an invalid old PID record must not let Qt remove a live flock.
        QLockFile oldWaiter(path);
        QVERIFY(!oldWaiter.tryLock(0));
        struct stat after {};
        QVERIFY(::lstat(QFile::encodeName(path).constData(), &after) == 0);
        QCOMPARE(after.st_dev, before.st_dev);
        QCOMPARE(after.st_ino, before.st_ino);
        QVERIFY(stale.open(QIODevice::ReadOnly));
        QCOMPARE(stale.readAll(), contents);
        stale.close();
        current.release();
        QVERIFY(QFileInfo::exists(path));
    }

    void runtimeStartupLockCrashReleasesWithoutUnlink()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("viewer.launch"));
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LICASA_TEST_PROCESS_FIXTURE"),
                           QStringLiteral("runtime-lock-holder"));
        environment.insert(QStringLiteral("LICASA_TEST_RUNTIME_LOCK_PATH"), path);
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath());
        QVERIFY(child.waitForReadyRead(5000));
        QCOMPARE(child.readAllStandardOutput(), QByteArray("ready\n"));
        struct stat original {};
        QVERIFY(::lstat(QFile::encodeName(path).constData(), &original) == 0);
        Licasa::RuntimeStartupLock contender;
        QString reason;
        const bool blocked = !contender.acquire(path, &reason, 0);
        child.kill(); // Crash only this owned lock fixture, never a viewer.
        QVERIFY(child.waitForFinished(5000));
        QCOMPARE(child.exitStatus(), QProcess::CrashExit);
        QVERIFY(blocked);
        QVERIFY2(contender.acquire(path, &reason, 0), qPrintable(reason));
        struct stat retained {};
        QVERIFY(::lstat(QFile::encodeName(path).constData(), &retained) == 0);
        QCOMPARE(retained.st_dev, original.st_dev);
        QCOMPARE(retained.st_ino, original.st_ino);
    }

    void runtimeStartupLockDoesNotLeakAcrossExec()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("viewer.launch"));
        Licasa::RuntimeStartupLock first;
        QString reason;
        QVERIFY2(first.acquire(path, &reason), qPrintable(reason));
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LICASA_TEST_PROCESS_FIXTURE"), QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath());
        QVERIFY(child.waitForReadyRead(5000));
        QCOMPARE(child.readAllStandardOutput(), QByteArray("ready\n"));
        first.release();
        Licasa::RuntimeStartupLock next;
        const bool acquired = next.acquire(path, &reason, 0);
        child.closeWriteChannel();
        QVERIFY(child.waitForFinished(5000));
        QCOMPARE(child.exitCode(), 0);
        QVERIFY2(acquired, qPrintable(reason));
    }

    void runtimeStartupLockRejectsUnsafePaths_data()
    {
        QTest::addColumn<QString>("unsafe");
        for (const char* name : {"symlink", "hardlink", "fifo", "group-readable", "group-writable",
                                 "executable", "public-parent", "symlink-parent"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    void runtimeStartupLockRejectsUnsafePaths()
    {
        QFETCH(QString, unsafe);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString directory = temporary.filePath(QStringLiteral("runtime"));
        QVERIFY(QDir().mkdir(directory));
        QVERIFY(::chmod(QFile::encodeName(directory).constData(), 0700) == 0);
        QString path = QDir(directory).filePath(QStringLiteral("viewer.launch"));
        if (unsafe == QStringLiteral("public-parent")) {
            QVERIFY(::chmod(QFile::encodeName(directory).constData(), 0755) == 0);
        } else if (unsafe == QStringLiteral("symlink-parent")) {
            const QString alias = temporary.filePath(QStringLiteral("alias"));
            QVERIFY(QFile::link(directory, alias));
            path = QDir(alias).filePath(QStringLiteral("viewer.launch"));
        } else if (unsafe == QStringLiteral("fifo")) {
            QVERIFY(::mkfifo(QFile::encodeName(path).constData(), 0600) == 0);
        } else {
            const QString target =
                unsafe == QStringLiteral("symlink") || unsafe == QStringLiteral("hardlink")
                    ? QDir(directory).filePath(QStringLiteral("target"))
                    : path;
            QFile file(target);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("unchanged"), qint64(9));
            file.close();
            mode_t mode = 0600;
            if (unsafe == QStringLiteral("group-readable")) {
                mode = 0640;
            } else if (unsafe == QStringLiteral("group-writable")) {
                mode = 0660;
            } else if (unsafe == QStringLiteral("executable")) {
                mode = 0700;
            }
            QVERIFY(::chmod(QFile::encodeName(target).constData(), mode) == 0);
            if (unsafe == QStringLiteral("symlink")) {
                QVERIFY(QFile::link(target, path));
            } else if (unsafe == QStringLiteral("hardlink")) {
                QVERIFY(::link(QFile::encodeName(target).constData(),
                               QFile::encodeName(path).constData()) == 0);
            }
        }
        Licasa::RuntimeStartupLock lock;
        QString reason;
        QVERIFY(!lock.acquire(path, &reason, 0));
        QVERIFY(!reason.isEmpty());
    }

    void runtimeStartupLockRejectsReplacedPath_data()
    {
        QTest::addColumn<bool>("replaceParent");
        QTest::newRow("file-inode") << false;
        QTest::newRow("parent-inode") << true;
    }

    void runtimeStartupLockRejectsReplacedPath()
    {
        QFETCH(bool, replaceParent);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString directory = temporary.filePath(QStringLiteral("runtime"));
        QVERIFY(QDir().mkdir(directory));
        QVERIFY(::chmod(QFile::encodeName(directory).constData(), 0700) == 0);
        const QString path = QDir(directory).filePath(QStringLiteral("viewer.launch"));
        QFile blocker(path);
        QVERIFY(blocker.open(QIODevice::ReadWrite));
        QVERIFY(::fchmod(blocker.handle(), 0600) == 0);
        QVERIFY(::fcntl(blocker.handle(), F_SETFD, FD_CLOEXEC) == 0);
        QVERIFY(::flock(blocker.handle(), LOCK_EX | LOCK_NB) == 0);
        struct stat original {};
        QVERIFY(::fstat(blocker.handle(), &original) == 0);
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LICASA_TEST_PROCESS_FIXTURE"),
                           QStringLiteral("runtime-lock-waiter"));
        environment.insert(QStringLiteral("LICASA_TEST_RUNTIME_LOCK_PATH"), path);
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath());
        QVERIFY(child.waitForReadyRead(5000));
        QCOMPARE(child.readAllStandardOutput(), QByteArray("waiting\n"));
        QTRY_VERIFY_WITH_TIMEOUT(processHoldsInode(child.processId(), original), 1000);
        if (replaceParent) {
            QVERIFY(QDir().rename(directory, directory + QStringLiteral(".old")));
            QVERIFY(QDir().mkdir(directory));
            QVERIFY(::chmod(QFile::encodeName(directory).constData(), 0700) == 0);
        } else {
            QVERIFY(QFile::rename(path, path + QStringLiteral(".old")));
        }
        QFile replacement(path);
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        QVERIFY(::fchmod(replacement.handle(), 0600) == 0);
        replacement.close();
        QVERIFY(::flock(blocker.handle(), LOCK_UN) == 0);
        QVERIFY(child.waitForFinished(5000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 40);
        QVERIFY(child.readAllStandardOutput().contains("path changed"));
        QVERIFY(QFileInfo::exists(path));
        Licasa::RuntimeStartupLock retry;
        QString reason;
        QVERIFY2(retry.acquire(path, &reason, 0), qPrintable(reason));
    }

    void resultCodesNeverTreatUnknownAsSuccess()
    {
        QCOMPARE(Licasa::lifecycleResultExitCode(
                     {{QStringLiteral("result"), QStringLiteral("accepted")}}),
                 0);
        QCOMPARE(
            Licasa::lifecycleResultExitCode({{QStringLiteral("result"), QStringLiteral("absent")}}),
            3);
        QCOMPARE(
            Licasa::lifecycleResultExitCode({{QStringLiteral("result"), QStringLiteral("busy")}}),
            4);
        QCOMPARE(Licasa::lifecycleResultExitCode(
                     {{QStringLiteral("result"), QStringLiteral("invented")}}),
                 9);
    }

    void concurrentControlClientDoesNotBlockStartup_data()
    {
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<bool>("blocksStartup");
        for (const QString& operation :
             {QStringLiteral("identity"), QStringLiteral("status"), QStringLiteral("health"),
              QStringLiteral("prepare-update"), QStringLiteral("stop-for-update"),
              QStringLiteral("cancel-update"), QStringLiteral("quit")}) {
            QTest::newRow(qPrintable(operation))
                << QStringList{QStringLiteral("--lifecycle"), operation} << false;
        }
        QTest::newRow("quit-alias") << QStringList{QStringLiteral("--quit")} << false;
        QTest::newRow("ordinary-viewer") << QStringList{} << true;
        QTest::newRow("unknown-operation")
            << QStringList{QStringLiteral("--lifecycle"), QStringLiteral("unknown")} << true;
        QTest::newRow("missing-operation") << QStringList{QStringLiteral("--lifecycle")} << true;
        QTest::newRow("extra-arguments")
            << QStringList{QStringLiteral("--lifecycle"), QStringLiteral("status"),
                           QStringLiteral("photo.jpg")}
            << true;
        QTest::newRow("mixed-quit-viewer")
            << QStringList{QStringLiteral("--quit"), QStringLiteral("photo.jpg")} << true;
    }

    void concurrentControlClientDoesNotBlockStartup()
    {
        QFETCH(QStringList, arguments);
        QFETCH(bool, blocksStartup);
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LICASA_TEST_PROCESS_FIXTURE"), QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(), arguments);
        QVERIFY(child.waitForReadyRead(5000));
        QCOMPARE(child.readAllStandardOutput(), QByteArray("ready\n"));
        const bool blocked = Licasa::sameInstallationProcessExists(nullptr, false);
        child.closeWriteChannel();
        QVERIFY(child.waitForFinished(5000));
        QCOMPARE(child.exitCode(), 0);
        QCOMPARE(blocked, blocksStartup);
    }

    void inaccessibleSameUserViewerCannotBeOmittedByProcDirectoryOwner()
    {
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LICASA_TEST_PROCESS_FIXTURE"),
                           QStringLiteral("inaccessible-viewer"));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath());
        QVERIFY(child.waitForReadyRead(5000));
        QCOMPARE(child.readAllStandardOutput(), QByteArray("ready\n"));
        const qint64 fixturePid = child.processId();
        const bool inaccessible = Licasa::processExecutablePath(fixturePid).isEmpty();
        QString reason;
        const bool blocked = Licasa::sameInstallationProcessExists(&reason);
        const bool guiBlocked = Licasa::sameInstallationProcessExists(nullptr, false);
        int unknownProcessCount = 0;
        const bool scopedBlocked =
            Licasa::scopedInstallationProcessExists(nullptr, &unknownProcessCount);
        child.closeWriteChannel();
        QVERIFY(child.waitForFinished(5000));
        QCOMPARE(child.exitCode(), 0);
        if (!inaccessible) {
            QSKIP("Host privileges permit inspecting nondumpable peers; denial path requires an "
                  "unprivileged process.");
        }
        QVERIFY(blocked);
        QVERIFY(!guiBlocked);
        QVERIFY(!scopedBlocked);
        QVERIFY(unknownProcessCount >= 1);
        QVERIFY2(reason.contains(QStringLiteral("inspection is unavailable")), qPrintable(reason));
        if (qEnvironmentVariableIsSet("LICASA_TEST_ISOLATED_PROC")) {
            QVERIFY2(reason.contains(QStringLiteral("process %1;").arg(fixturePid)),
                     qPrintable(reason));
        }
    }

    void liveControlClientCannotProveScopedAbsence()
    {
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LICASA_TEST_PROCESS_FIXTURE"), QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--lifecycle"), QStringLiteral("status")});
        QVERIFY(child.waitForReadyRead(5000));
        QCOMPARE(child.readAllStandardOutput(), QByteArray("ready\n"));
        const qint64 fixturePid = child.processId();
        QString reason;
        bool controlClientPending = false;
        const bool scopedBlocked =
            Licasa::scopedInstallationProcessExists(&reason, nullptr, &controlClientPending);
        const bool strictBlocked = Licasa::sameInstallationProcessExists();
        const bool guiBlocked = Licasa::sameInstallationProcessExists(nullptr, false);
        child.closeWriteChannel();
        QVERIFY(child.waitForFinished(5000));
        QCOMPARE(child.exitCode(), 0);
        QVERIFY(scopedBlocked);
        QVERIFY(controlClientPending);
        QVERIFY(strictBlocked);
        QVERIFY(!guiBlocked);
        QVERIFY2(reason.contains(QStringLiteral("(%1)").arg(fixturePid)), qPrintable(reason));
        QVERIFY(!Licasa::scopedInstallationProcessExists());
    }

    void scopedCliReportsBusyAndNoGlobalAbsenceClaim()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString copy = temporary.filePath(QStringLiteral("scoped-cli-peer"));
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), copy));
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), temporary.path());
        environment.insert(QStringLiteral("LICASA_TEST_PROCESS_FIXTURE"), QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.start(copy, {QStringLiteral("--lifecycle"), QStringLiteral("status")});
        QVERIFY(child.waitForReadyRead(5000));
        QCOMPARE(child.readAllStandardOutput(), QByteArray("ready\n"));
        QProcess client;
        environment.insert(QStringLiteral("LICASA_TEST_PROCESS_FIXTURE"),
                           QStringLiteral("scoped-absence-cli"));
        client.setProcessEnvironment(environment);
        client.start(copy);
        const bool finished = client.waitForFinished(5000);
        const QByteArray output = client.readAllStandardOutput();
        child.closeWriteChannel();
        QVERIFY(child.waitForFinished(5000));
        QVERIFY(finished);
        QCOMPARE(client.exitCode(), 4);
        const auto reply = QJsonDocument::fromJson(output).object();
        QCOMPARE(reply.value(QStringLiteral("result")).toString(), QStringLiteral("busy"));
        QCOMPARE(reply.value(QStringLiteral("inspectionScope")).toString(),
                 QStringLiteral("installation"));
        QVERIFY(reply.contains(QStringLiteral("unknownProcessCount")));
        QCOMPARE(reply.value(QStringLiteral("globalAbsenceProven")).toBool(true), false);
        QVERIFY(!reply.value(QStringLiteral("busyReason")).toString().isEmpty());
    }

    void sameExecutableWithoutWritePermissionRequiresIndependentDigest()
    {
        QVERIFY2(::geteuid() != 0, "The EACCES regression requires desktop credentials.");
        // Running the same ELF is not sufficient to reuse a receipt when the
        // write probe fails on permissions instead of proving ETXTBSY.
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString copy = temporary.filePath(QStringLiteral("readonly-lifecycle-peer"));
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), copy));
        QVERIFY(QFile::setPermissions(copy, QFile::ReadOwner | QFile::ExeOwner | QFile::ReadGroup |
                                                QFile::ExeGroup | QFile::ReadOther |
                                                QFile::ExeOther));
        QProcess client;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LICASA_TEST_PROCESS_FIXTURE"),
                           QStringLiteral("conservative-hash-client"));
        client.setProcessEnvironment(environment);
        client.start(copy);
        QVERIFY(client.waitForFinished(10000));
        const QByteArray diagnostics = client.readAllStandardError();
        QCOMPARE(client.exitStatus(), QProcess::NormalExit);
        QVERIFY2(client.exitCode() == 0, diagnostics.constData());
    }

  private:
    static bool processHoldsInode(qint64 pid, const struct stat& expected)
    {
        const QString directory = QStringLiteral("/proc/%1/fd").arg(pid);
        for (const QString& entry :
             QDir(directory).entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
            struct stat status {};
            if (::stat(QFile::encodeName(QDir(directory).filePath(entry)).constData(), &status) ==
                    0 &&
                status.st_dev == expected.st_dev && status.st_ino == expected.st_ino) {
                return true;
            }
        }
        return false;
    }

    QMap<QByteArray, QByteArray> environment_;
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QString fixture = qEnvironmentVariable("LICASA_TEST_PROCESS_FIXTURE");
    if (fixture == QStringLiteral("runtime-lock-holder") ||
        fixture == QStringLiteral("runtime-lock-waiter")) {
        if (fixture == QStringLiteral("runtime-lock-waiter")) {
            QTextStream(stdout) << "waiting\n" << Qt::flush;
        }
        Licasa::RuntimeStartupLock lock;
        QString reason;
        if (!lock.acquire(qEnvironmentVariable("LICASA_TEST_RUNTIME_LOCK_PATH"), &reason)) {
            QTextStream(stdout) << "refused: " << reason << '\n' << Qt::flush;
            return 40;
        }
        if (fixture == QStringLiteral("runtime-lock-waiter")) {
            return 0;
        }
        QTextStream(stdout) << "ready\n" << Qt::flush;
        char byte;
        while (::read(STDIN_FILENO, &byte, 1) > 0) {
        }
        return 0;
    }
    if (fixture == QStringLiteral("scoped-absence-cli")) {
        return Licasa::runLifecycleControl(QStringLiteral("status"));
    }
    if (fixture == QStringLiteral("conservative-hash-server")) {
        QLocalServer server;
        QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
        QObject::connect(&input, &QSocketNotifier::activated, &application, [] {
            char byte;
            if (::read(STDIN_FILENO, &byte, 1) <= 0) {
                QCoreApplication::exit(0);
            }
        });
        server.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server.listen(Licasa::lifecycleEndpointName())) {
            return 20;
        }
        QObject::connect(&server, &QLocalServer::newConnection, &application, [&] {
            while (server.hasPendingConnections()) {
                QLocalSocket* socket = server.nextPendingConnection();
                const auto respond = [socket] {
                    if (!socket->canReadLine()) {
                        return;
                    }
                    socket->readLine();
                    auto reply = Licasa::applicationIdentity().toJson();
                    reply.insert(QStringLiteral("generation"), QString(64, QLatin1Char('a')));
                    reply.insert(QStringLiteral("result"), QStringLiteral("ready"));
                    socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
                    socket->disconnectFromServer();
                };
                QObject::connect(socket, &QLocalSocket::readyRead, socket, respond);
                QObject::connect(socket, &QLocalSocket::disconnected, socket,
                                 &QObject::deleteLater);
                respond();
            }
        });
        QTimer::singleShot(5000, &application, [] { QCoreApplication::exit(21); });
        const char ready[] = "ready\n";
        if (::write(STDOUT_FILENO, ready, sizeof(ready) - 1) != sizeof(ready) - 1) {
            return 22;
        }
        return application.exec();
    }
    if (fixture == QStringLiteral("conservative-hash-client")) {
        const int descriptor = ::open("/proc/self/exe", O_WRONLY | O_CLOEXEC);
        if (descriptor >= 0) {
            ::close(descriptor);
            qCritical("Readonly fixture unexpectedly permits writing");
            return 30;
        }
        if (errno != EACCES ||
            Licasa::canReuseExecutableGeneration(QCoreApplication::applicationPid(),
                                                 QCoreApplication::applicationPid())) {
            qCritical("Fixture did not establish the conservative EACCES path");
            return 31;
        }
        QTemporaryDir runtime;
        if (!runtime.isValid()) {
            return 32;
        }
        qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        QProcess server;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LICASA_TEST_PROCESS_FIXTURE"),
                           QStringLiteral("conservative-hash-server"));
        server.setProcessEnvironment(environment);
        server.start(QCoreApplication::applicationFilePath());
        if (!server.waitForReadyRead(5000) || server.readAllStandardOutput() != "ready\n") {
            qCritical("Real lifecycle peer failed to become ready");
            return 33;
        }
        const auto reply = Licasa::lifecycleRequest(QStringLiteral("status"));
        const bool rejected = reply.value(QStringLiteral("result")) == QStringLiteral("broken");
        server.closeWriteChannel();
        if (!server.waitForFinished(5000) || server.exitCode() != 0 || !rejected) {
            qCritical().noquote() << "Nonwritable same-object peer receipt was not rejected:"
                                  << QJsonDocument(reply).toJson(QJsonDocument::Compact);
            return 34;
        }
        return 0;
    }
    if (fixture == QStringLiteral("1") || fixture == QStringLiteral("inaccessible-viewer")) {
        // Real same-executable peers, kept alive until the parent closes stdin.
        // This branch is confined to the test executable.
        if (fixture == QStringLiteral("inaccessible-viewer") &&
            (::prctl(PR_SET_DUMPABLE, 0) != 0 || ::prctl(PR_SET_NAME, "unrelated") != 0)) {
            return 2;
        }
        const char ready[] = "ready\n";
        const auto written = ::write(STDOUT_FILENO, ready, sizeof(ready) - 1);
        if (written != sizeof(ready) - 1) {
            return 1;
        }
        char byte;
        while (::read(STDIN_FILENO, &byte, 1) > 0) {
        }
        return 0;
    }
    LifecyclePlatformTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tst_lifecycle_platform.moc"
