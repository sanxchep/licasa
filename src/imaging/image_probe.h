#pragma once

#include <QObject>
#include <QSize>
#include <QUrl>
#include <QVariantMap>

namespace Licasa {

class ImageResourcePolicy;

class ImageProbe final : public QObject {
    Q_OBJECT

  public:
    explicit ImageProbe(ImageResourcePolicy& resourcePolicy, QObject* parent = nullptr);

    // One metadata snapshot keeps size, animation, budget and file bytes in
    // agreement, and avoids repeated filesystem checks at the QML boundary.
    Q_INVOKABLE QVariantMap inspect(const QUrl& url) const;

  private:
    mutable QString lastProbePath_;
    mutable qint64 lastProbeBytes_ = -1;
    mutable qint64 lastProbeModified_ = -1;
    mutable quint64 lastProbeBudget_ = 0;
    mutable QVariantMap lastProbe_;
    ImageResourcePolicy& resourcePolicy_;
};

} // namespace Licasa
