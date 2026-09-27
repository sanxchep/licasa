#pragma once

#include <QObject>

class PhotoAssetProbeTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void plainJpegHasNoMotion();
    void motionJpegIsDetectedFromStandardApp1Xmp();
    void legacyPixelMicroVideoJpegIsDetectedFromStandardApp1Xmp();
    void samsungSeftJpegWithoutXmpIsDetected();
    void samsungSeftTrimsClassicMicroVideoFraming();
    void samsungSeftCanRecoverMalformedModernMetadata();
    void samsungSeftDoesNotOverrideExplicitMotionPhotoZero();
    void samsungSeftWithoutMotionRecordStaysStill();
    void scanStopsBeforeCompressedImageData();
    void malformedMetadataIsBoundedAndNonFatal();
    void metadataScanLimitsAreEnforced();
    void asynchronousProbeEmitsSimpleCapabilityState();
    void rapidNavigationSuppressesStaleResult();
    void appleJpegPairIsIntegratedAsExternalMotion();
    void appleJpegMismatchedPairStaysStill();
    void asynchronousApplePairingEmitsCapability();
#if defined(LICASA_APPLE_REAL_HEIC_FIXTURE) && defined(LICASA_APPLE_REAL_MOV_FIXTURE)
    void realAppleHeicMovPairIsIntegrated();
#endif
#ifdef LICASA_LEGACY_PIXEL_REAL_FIXTURE
    void realLegacyPixelMvimgIsIntegrated();
#endif
#if defined(LICASA_SAMSUNG_REAL_FIXTURE_1) && defined(LICASA_SAMSUNG_REAL_FIXTURE_2) &&            \
    defined(LICASA_SAMSUNG_REAL_FIXTURE_3)
    void realSamsungSeftFixturesAreIntegrated();
#endif
#if defined(LICASA_XIAOMI_REAL_FIXTURE_MODERN) && defined(LICASA_XIAOMI_REAL_FIXTURE_LEGACY) &&    \
    defined(LICASA_XIAOMI_REAL_FIXTURE_GAINMAP)
    void realXiaomiMotionFixturesAreIntegrated();
#endif
#ifdef LICASA_HEIC_MOTION_FIXTURE
    void realHeicMotionPhotoBridge();
#endif
#ifdef LICASA_AVIF_XMP_FIXTURE
    void realAvifXmpRemainsStill();
#endif
};
