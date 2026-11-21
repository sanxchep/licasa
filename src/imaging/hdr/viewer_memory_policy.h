#pragma once
#include "viewer_resource_budget.h"
#include "../../app/application_settings.h"
#include <QGuiApplication>
#include <QQuickWindow>

namespace Licasa {
inline void bindHdrMemoryPolicy(ViewerPreferences& preferences)
{
    configureHdrMemoryBudget(preferences.maximumImageMemoryMiB());
    QObject::connect(&preferences, &ViewerPreferences::maximumImageMemoryMiBChanged, &preferences,
        [&preferences] {
            configureHdrMemoryBudget(preferences.maximumImageMemoryMiB());
            for (auto* window : QGuiApplication::allWindows())
                if (auto* quick = qobject_cast<QQuickWindow*>(window); quick && quick->isVisible()) quick->update();
        });
}
}
