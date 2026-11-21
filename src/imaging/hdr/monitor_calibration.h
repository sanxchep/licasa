#pragma once
#include "output_policy.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QVariantMap>

namespace Licasa {
inline QString hdrMonitorKey(QString manufacturer, QString model, QString serial)
{
    manufacturer = manufacturer.trimmed(); model = model.trimmed(); serial = serial.trimmed();
    const auto lower = serial.toLower();
    if (manufacturer.isEmpty() || model.isEmpty() || serial.isEmpty()
        || lower == "unknown" || lower == "n/a" || lower == "none"
        || serial.count('0') == serial.size()) return {};
    const auto bytes = QJsonDocument(QJsonArray{manufacturer, model, serial}).toJson(QJsonDocument::Compact);
    return QStringLiteral("v1-") + QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
}
inline bool validHdrMonitorKey(const QString& key)
{
    if (key.size()!=67 || !key.startsWith("v1-")) return false;
    for (auto c : key.mid(3)) if (!((c>='0' && c<='9') || (c>='a' && c<='f'))) return false;
    return true;
}
inline bool validHdrCalibrationProfile(const QVariantMap& profile)
{
    if (profile.value("version").toInt()!=1 || profile.value("enabled").typeId()!=QMetaType::Bool) return false;
    bool whiteOk=false, peakOk=false;
    const auto white=profile.value("paperWhite").toFloat(&whiteOk);
    const auto peak=profile.value("peak").toFloat(&peakOk);
    return whiteOk && peakOk && calibratedHdrOutput(white,peak,true).headroom>1;
}
}
