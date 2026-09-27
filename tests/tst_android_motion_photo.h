#pragma once

#include <QObject>

class AndroidMotionPhotoTest final : public QObject {
    Q_OBJECT

  private slots:
    void validJpegUsesFinalItemLengthAndBoundedRange();
    void quickTimeWidePreambleIsAccepted();
    void legacyPixelMicroVideoUsesBackwardOffsetAndBoundedRange();
    void legacyPixelCameraPrefixAndOptionalVersionAreAccepted();
    void legacyPixelInvalidMetadataIsRejected();
    void modernDirectoryTakesPrecedenceOverLegacyOffset();
    void modernDeclarationDoesNotFallBackToLegacy();
    void samsungSeftMotionPhotoUsesIndexedRecordRange();
    void modernSamsungSeftCoLocatedRangeTrimsVendorDirectory();
    void modernSamsungSeftDifferentStartRemainsModern();
    void samsungSeftWithoutMotionRecordStaysStill();
    void samsungSeftMalformedIndexAndPayloadAreRejected();
    void residualFlagWithoutActualVideoIsRejected();
    void nonOneMotionPhotoFlagIsNotMotionPhoto();
    void directoryRulesAndLengthOverflowAreRejected();
    void heicAndAvifRequireEightByteMpvdHeader();
    void oversizedOrEntityBearingXmpIsRejected();
    void cancellationStopsVideoValidation();
};
