#pragma once
#include <QQuickItem>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <memory>

namespace Licasa {
// Default builds retain a no-op item; the optional Qt 6.11 bridge is qualified
// separately. It never changes the SDR image provider or window surface format.
class HdrImageItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QString presentedRequest READ presentedRequest WRITE setPresentedRequest NOTIFY presentedRequestChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(float outputWhiteScale READ outputWhiteScale NOTIFY outputWhiteScaleChanged)
    Q_PROPERTY(bool outputReady READ outputReady NOTIFY outputWhiteScaleChanged)
    Q_PROPERTY(QVariantMap outputSettings READ outputSettings NOTIFY outputSettingsChanged)
    Q_PROPERTY(PhotoStatus photoStatus READ photoStatus NOTIFY photoStatusChanged)
public:
    enum class PhotoStatus { None, Sdr, Preparing, Unavailable, Hdr };
    Q_ENUM(PhotoStatus)
    explicit HdrImageItem(QQuickItem* parent = nullptr);
    ~HdrImageItem() override;
    QUrl source() const { return source_; }
    // The image request whose SDR base is on screen. The HDR rendition is
    // reconstructed only from the file that produced that base.
    QString presentedRequest() const { return presentedRequest_; }
    void setPresentedRequest(const QString& request);
    bool active() const { return active_; }
    // Zero means the native surface has not qualified for linear HDR output.
    float outputWhiteScale() const;
    bool outputReady() const;
    QVariantMap outputSettings() const;
    PhotoStatus photoStatus() const;
    Q_INVOKABLE bool saveHdrCalibration(const QString& revision, float white, float peak, bool remember);
    Q_INVOKABLE bool useSdr(const QString& revision);
    Q_INVOKABLE bool forgetHdrCalibration(const QString& revision);
    void setSource(const QUrl& source);
    void setActive(bool active);
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    // A render-owner receipt for the optional viewer diagnostics.
    quint64 visibleHdrFrames() const;
#endif
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
    static void armRetirementDiagnostic(QQuickWindow* window);
    static void releaseDiagnosticFence();
    static int diagnosticRasterRefusals();
#endif
#ifdef LICASA_HDR_RENDER_TEST
    // Not a QML property, not compiled into the application: redirected FP16
    // render targets qualify ownership/pixels without claiming a real output.
    void setTestHeadroom(float headroom);
    void setTestPaperWhite(float paperWhite, float peak);
    QVariantMap testUsage() const;
    std::shared_ptr<void> testHoldGpu(quint64 bytes);
    // Faults for the next render-thread steps of any item in the process.
    // textureFailures: that many texture creations return nothing.
    // holdFences: retirement fences stay pending while set.
    // loseFence: the next retirement fence reports a lost device.
    struct TestFaults {
        int textureFailures = 0;
        bool holdFences = false;
        bool loseFence = false;
    };
    static void setTestFaults(TestFaults faults);
    static int testPendingTextureFailures();
#endif
signals:
    void sourceChanged();
    void presentedRequestChanged();
    void activeChanged();
    void outputWhiteScaleChanged();
    void outputSettingsChanged();
    void photoStatusChanged();
protected:
    bool eventFilter(QObject*, QEvent*) override;
    void geometryChange(const QRectF&, const QRectF&) override;
    QSGNode* updatePaintNode(QSGNode*, UpdatePaintNodeData*) override;
private:
#ifdef LICASA_ENABLE_HDR_VIEWER_LAB
    void setPhotoStatus(PhotoStatus status);
    bool pinSource();
#endif
#ifdef LICASA_HDR_RETRY_DIAGNOSTIC
    void recordRetryDiagnostic(const char* event, int retry) const;
#endif
    void refresh();
    void tryUpload();
    void observeWindow(QQuickWindow* window);
    struct State;
    std::unique_ptr<State> state_;
    QUrl source_;
    QString presentedRequest_;
    bool active_ = false;
};
} // namespace Licasa
