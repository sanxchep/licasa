#pragma once

#include <QImage>
#include <QList>
#include <QPoint>
#include <QStringList>

#include <optional>

class QJsonObject;

namespace PhotoReadback {
std::optional<QList<QPoint>> samplePoints(QSize size, const QStringList& coordinates);
bool verify(const QImage& frame, const QImage& upload, float backing, const QList<QPoint>& points,
            QJsonObject& details);
} // namespace PhotoReadback
