#include "app/app_constants.h"
#include "platform/single_instance_endpoint.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace Licasa;

class SingleInstanceEndpointTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase()
    {
        previousRuntime_ = qgetenv("XDG_RUNTIME_DIR");
        runtime_ = std::make_unique<QTemporaryDir>();
        QVERIFY(runtime_->isValid());
        QVERIFY(QFile::setPermissions(runtime_->path(), QFileDevice::ReadOwner |
                                                            QFileDevice::WriteOwner |
                                                            QFileDevice::ExeOwner));
        QVERIFY(qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime_->path())));
    }

    void cleanupTestCase()
    {
        if (previousRuntime_.isNull()) {
            qunsetenv("XDG_RUNTIME_DIR");
        } else {
            qputenv("XDG_RUNTIME_DIR", previousRuntime_);
        }
    }

    void endpointUsesExplicitPrivateRuntimeDirectory()
    {
#if !defined(Q_OS_UNIX)
        QSKIP("Unix filesystem endpoint security test");
#else
        const QString endpoint = singleInstanceEndpointName();
        QVERIFY(!endpoint.isEmpty());
        QVERIFY(QDir::isAbsolutePath(endpoint));
        QCOMPARE(QFileInfo(endpoint).absolutePath(), QDir(runtime_->path()).absolutePath());
        QCOMPARE(QFileInfo(endpoint).fileName(),
                 QString::fromLatin1(Constants::singleInstanceServerName));
        QVERIFY(endpoint !=
                QDir(QDir::tempPath())
                    .absoluteFilePath(QString::fromLatin1(Constants::singleInstanceServerName)));
#endif
    }

    void runtimeValidationRejectsSharedAndSymlinkedDirectories()
    {
#if !defined(Q_OS_UNIX)
        QSKIP("Unix runtime directory validation test");
#else
        QVERIFY(Internal::isPrivateSingleInstanceRuntimeDirectory(runtime_->path()));
        QVERIFY(!Internal::isPrivateSingleInstanceRuntimeDirectory(QStringLiteral("relative")));

        const QString shared = runtime_->filePath(QStringLiteral("shared"));
        QVERIFY(QDir().mkdir(shared));
        QVERIFY(QFile::setPermissions(shared, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                                  QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                                  QFileDevice::ExeOther));
        QVERIFY(!Internal::isPrivateSingleInstanceRuntimeDirectory(shared));

        const QString link = runtime_->filePath(QStringLiteral("runtime-link"));
        QVERIFY(QFile::link(runtime_->path(), link));
        QVERIFY(!Internal::isPrivateSingleInstanceRuntimeDirectory(link));

        // A mode-0700 leaf is not private when another user can replace it
        // through a non-sticky shared parent.
        QTemporaryDir sharedTree;
        QVERIFY(sharedTree.isValid());
        QVERIFY(QFile::setPermissions(
            sharedTree.path(),
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther));
        const QString replaceableLeaf = QDir(sharedTree.path()).filePath(QStringLiteral("runtime"));
        QVERIFY(QDir().mkdir(replaceableLeaf));
        QVERIFY(QFile::setPermissions(replaceableLeaf, QFileDevice::ReadOwner |
                                                           QFileDevice::WriteOwner |
                                                           QFileDevice::ExeOwner));
        QVERIFY(!Internal::isPrivateSingleInstanceRuntimeDirectory(replaceableLeaf));
#endif
    }

    void missingRuntimeDirectoryDisablesEndpoint()
    {
#if !defined(Q_OS_UNIX)
        QSKIP("Unix runtime fallback test");
#else
        const QByteArray privateRuntime = qgetenv("XDG_RUNTIME_DIR");
        qunsetenv("XDG_RUNTIME_DIR");
        QVERIFY(singleInstanceEndpointName().isEmpty());
        QVERIFY(qputenv("XDG_RUNTIME_DIR", privateRuntime));
        QVERIFY(!singleInstanceEndpointName().isEmpty());
#endif
    }

    void localServerSocketIsOwnerOnly()
    {
#if !defined(Q_OS_UNIX)
        QSKIP("Unix local socket permissions test");
#else
        const QString endpoint = singleInstanceEndpointName();
        QVERIFY(!endpoint.isEmpty());
        QLocalServer::removeServer(endpoint);
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        QVERIFY2(server.listen(endpoint), qPrintable(server.errorString()));
        QCOMPARE(server.fullServerName(), endpoint);

        struct stat status {};
        const QByteArray encodedEndpoint = QFile::encodeName(endpoint);
        QCOMPARE(::lstat(encodedEndpoint.constData(), &status), 0);
        QVERIFY(S_ISSOCK(status.st_mode));
        QCOMPARE(status.st_uid, ::geteuid());
        QCOMPARE(status.st_mode & (S_IRWXG | S_IRWXO), mode_t(0));

        QLocalSocket client;
        client.connectToServer(endpoint);
        QVERIFY2(client.waitForConnected(1000), qPrintable(client.errorString()));
        client.disconnectFromServer();
        if (client.state() != QLocalSocket::UnconnectedState) {
            client.waitForDisconnected(1000);
        }
        server.close();
        QLocalServer::removeServer(endpoint);
#endif
    }

  private:
    QByteArray previousRuntime_;
    std::unique_ptr<QTemporaryDir> runtime_;
};

QTEST_GUILESS_MAIN(SingleInstanceEndpointTest)

#include "tst_single_instance_endpoint.moc"
