#include "wayland_test_client.h"
#include <QtTest>

using Licasa::WaylandTest::Client;
using Licasa::WaylandTest::Config;

class WaylandHdrTests : public QObject {
    Q_OBJECT
private slots:
    void acceptedAndChanges() {
        Client client;
        QTRY_COMPARE(client.observer->report().state,Licasa::WaylandHdrReport::State::Ready);
        auto first=client.observer->report();
        QVERIFY(first.protocol && first.descriptionAccepted); QCOMPARE(first.referenceNits,80.0); QCOMPARE(first.targetNits,1000.0);
        QVERIFY(client.notifications>0); QCOMPARE(client.server.colorSurfaceRequests.load(),0);
        QCOMPARE(client.server.requestedTf.load(),uint32_t(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR));
        QCOMPARE(client.server.requestedPrimary.load(),uint32_t(WP_COLOR_MANAGER_V1_PRIMARIES_SRGB));
        QCOMPARE(client.server.requestedMaximum.load(),uint32_t(10000));
        client.server.nextMaximum=80;
        QTRY_COMPARE(client.observer->report().state,Licasa::WaylandHdrReport::State::Sdr);
        QVERIFY(client.observer->report().preferredChanged);
        QVERIFY(client.observer->report().revision>first.revision);
        client.server.nextMaximum=800;
        QTRY_COMPARE(client.observer->report().state,Licasa::WaylandHdrReport::State::Ready);
        QCOMPARE(client.observer->report().targetNits,800.0);
        client.server.removeGlobal=true;
        QTRY_COMPARE(client.observer->report().reason,QStringLiteral("color_manager_removed"));
        QVERIFY(!client.observer->report().descriptionAccepted);
        client.server.addGlobal=true;
        QTRY_COMPARE(client.observer->report().state,Licasa::WaylandHdrReport::State::Ready);
        QVERIFY(client.observer->report().preferredChanged); // Re-advertisement needs GUI review.
        client.server.disconnectPeer=true;
        QTRY_COMPARE(client.observer->report().reason,QStringLiteral("connection_failed"));
        QCOMPARE(client.server.colorSurfaceRequests.load(),0);
    }
    void equalTargetPrimariesMayBeOmitted() {
        Config config; config.omitTargetPrimaries=true;
        Client omitted(config);
        QTRY_COMPARE(omitted.observer->report().state,Licasa::WaylandHdrReport::State::Ready);
        Client explicitPrimaries;
        QTRY_COMPARE(explicitPrimaries.observer->report().state,Licasa::WaylandHdrReport::State::Ready);
        QCOMPARE(omitted.observer->report().preferredFingerprint,explicitPrimaries.observer->report().preferredFingerprint);
        QCOMPARE(omitted.server.colorSurfaceRequests.load(),0);
    }
    void explicitTagFollowsNativeSurface() {
        Config config; config.allowSurfaceTag=true;
        Client client(config);
        QVERIFY(!client.observer->tagLinearSurface());
        QTRY_COMPARE(client.observer->report().state,Licasa::WaylandHdrReport::State::Ready);
        QVERIFY(client.observer->tagLinearSurface());
        QVERIFY(client.observer->tagLinearSurface());
        QTRY_COMPARE(client.server.surfaceTags.load(),1);
        QVERIFY(client.observer->linearSurfaceTagged());
        const auto hdr=client.observer->report();
        client.observer->setSurface(nullptr);
        wl_surface_destroy(client.surface);
        QVERIFY(!client.observer->linearSurfaceTagged());
        QVERIFY(!client.observer->tagLinearSurface());
        QTRY_COMPARE(client.server.retiredColorSurfaces.load(),1);
        client.server.nextMaximum=80;
        QTRY_COMPARE(client.server.nextMaximum.load(),-1);
        client.surface=wl_compositor_create_surface(client.compositor);
        client.observer->setSurface(client.surface);
        QTRY_COMPARE(client.observer->report().state,Licasa::WaylandHdrReport::State::Sdr);
        const auto replacement=client.observer->report().surfaceEpoch;
        QVERIFY(replacement>hdr.surfaceEpoch);
        QVERIFY(!client.observer->tagLinearSurface());
        client.server.nextMaximum=1000;
        QTRY_COMPARE(client.observer->report().state,Licasa::WaylandHdrReport::State::Ready);
        QCOMPARE(client.observer->report().surfaceEpoch,replacement);
        QCOMPARE(client.observer->report().preferredFingerprint,hdr.preferredFingerprint);
        QVERIFY(!client.observer->linearSurfaceTagged());
        QVERIFY(client.observer->tagLinearSurface());
        QTRY_COMPARE(client.server.surfaceTags.load(),2);
        QCOMPARE(wl_display_get_error(client.display),0);
    }
    void refusals_data() {
        QTest::addColumn<int>("kind"); QTest::addColumn<QString>("reason");
        QTest::newRow("missing-protocol")<<0<<QString("no_color_manager");
        QTest::newRow("missing-linear")<<1<<QString("linear_hdr_not_advertised");
        QTest::newRow("incomplete-capabilities")<<2<<QString("protocol_timeout");
        QTest::newRow("rejected-description")<<3<<QString("description_rejected");
        QTest::newRow("malformed-preferred")<<4<<QString("invalid_preferred_information");
        QTest::newRow("missing-preferred-response")<<5<<QString("protocol_timeout");
        QTest::newRow("missing-parametric")<<6<<QString("linear_hdr_not_advertised");
        QTest::newRow("missing-extended-volume")<<7<<QString("linear_hdr_not_advertised");
        QTest::newRow("missing-srgb")<<8<<QString("linear_hdr_not_advertised");
        QTest::newRow("missing-acceptance-response")<<9<<QString("protocol_timeout");
        QTest::newRow("invalid-acceptance-identity")<<10<<QString("invalid_description_identity");
        QTest::newRow("missing-intent")<<11<<QString("linear_hdr_not_advertised");
    }
    void refusals() {
        QFETCH(int,kind); QFETCH(QString,reason);
        Config config;
        if (kind==0) config.advertised=false;
        if (kind==1) config.linear=false;
        if (kind==2) config.complete=false;
        if (kind==3) config.accept=false;
        if (kind==4) config.malformed=true;
        if (kind==5) config.hang=true;
        if (kind==6) config.params=false;
        if (kind==7) config.extended=false;
        if (kind==8) config.primary=false;
        if (kind==9) config.hangAcceptance=true;
        if (kind==10) config.zeroIdentity=true;
        if (kind==11) config.intent=false;
        Client client(config);
        QTRY_COMPARE_WITH_TIMEOUT(client.observer->report().state,Licasa::WaylandHdrReport::State::Unavailable,5000);
        QCOMPARE(client.observer->report().reason,reason);
        QCOMPARE(client.server.colorSurfaceRequests.load(),0);
        if (kind<3 || kind==6 || kind==7 || kind==8 || kind==11) QCOMPARE(client.server.parametricRequests.load(),0);
    }
    void staleInformationCannotReplaceNewPreference() {
        Config config; config.delayInfo=true;
        Client client(config);
        QTRY_VERIFY(client.server.infoRequests.load()>0);
        client.server.nextMaximum=500;
        QTRY_COMPARE(client.observer->report().state,Licasa::WaylandHdrReport::State::Ready);
        const auto current=client.observer->report(); QCOMPARE(current.targetNits,500.0);
        client.server.releaseLate=true;
        QTest::qWait(100);
        QCOMPARE(client.observer->report().revision,current.revision);
        QCOMPARE(client.observer->report().preferredFingerprint,current.preferredFingerprint);
        QCOMPARE(client.server.colorSurfaceRequests.load(),0);
    }
    void surfaceRetirementAndClose() {
        Client client;
        QTRY_COMPARE(client.observer->report().state,Licasa::WaylandHdrReport::State::Ready);
        const auto original=client.observer->report();
        client.observer->setSurface(nullptr);
        wl_surface_destroy(client.surface); client.surface=nullptr;
        QTest::qWait(2200); // Hidden windows must not permanently time out.
        QCOMPARE(client.observer->report().state,Licasa::WaylandHdrReport::State::Pending);
        // Protocol identities may be reused after old descriptions die. A
        // recreated surface must retain a full information fingerprint too.
        client.server.reuseNextIdentity=true; client.server.nextMaximum=500;
        QTest::qWait(30);
        client.surface=wl_compositor_create_surface(client.compositor);
        client.observer->setSurface(client.surface);
        QTRY_COMPARE(client.observer->report().state,Licasa::WaylandHdrReport::State::Ready);
        QVERIFY(!client.observer->report().preferredChanged);
        QCOMPARE(client.observer->report().preferredIdentity,original.preferredIdentity);
        QVERIFY(client.observer->report().preferredFingerprint!=original.preferredFingerprint);
        client.server.nextMaximum=600;
        client.observer.reset(); // In-flight notifications cannot access a closed owner.
        QTest::qWait(30);
        QCOMPARE(client.server.colorSurfaceRequests.load(),0);
    }
};
QTEST_GUILESS_MAIN(WaylandHdrTests)
#include "tst_wayland_hdr.moc"
