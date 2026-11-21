#pragma once
#include "../gainmap-lab/resource_budget.h"
#include <QImage>
#include <QJsonObject>
class QRhi;
// Called on the QRhi/Vulkan owner thread outside an active QRhi frame.
bool checkNativeRetirement(QRhi* rhi, const QImage& source,
                           std::shared_ptr<void>& persistent,
                           const std::shared_ptr<Licasa::GainmapLab::ResourceBudget>& pool,
                           QJsonObject& details);
