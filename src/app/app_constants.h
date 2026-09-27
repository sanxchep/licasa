#pragma once

namespace Licasa::Constants {

inline constexpr char applicationName[] = "Licasa";
inline constexpr char organizationName[] = "Sanxchep";
inline constexpr char desktopFileName[] = "licasa";
inline constexpr char singleInstanceServerName[] = "licasa_singleton_v7";
inline constexpr char mainQmlUrl[] = "qrc:/Licasa/qml/Main.qml";
inline constexpr char imageProviderName[] = "licasa";
inline constexpr int minimumImageLimitMegapixels = 25;
inline constexpr int defaultImageLimitMegapixels = 536;
inline constexpr int maximumImageLimitMegapixels = 2147;
inline constexpr int minimumImageMemoryMiB = 384;
inline constexpr int defaultImageMemoryMiB = 8192;
inline constexpr int maximumImageMemoryMiB = 32768;
inline constexpr int estimatedWorkingBytesPerPixel = 16;
inline constexpr int decoderBudgetBytesPerPixel = 8;

} // namespace Licasa::Constants
