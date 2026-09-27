#pragma once

#include <QObject>
#include <QSet>
#include <QStringList>
#include <QUrl>

namespace Licasa {

class FormatSupport final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList nameFilters READ nameFilters CONSTANT)
    Q_PROPERTY(QStringList saveNameFilters READ saveNameFilters CONSTANT)

  public:
    explicit FormatSupport(QObject* parent = nullptr);

    QStringList nameFilters() const;
    QStringList saveNameFilters() const;
    Q_INVOKABLE bool canOpen(const QUrl& url) const;

  private:
    QSet<QString> readableExtensions_;
    QStringList nameFilters_;
    QStringList saveNameFilters_;
};

} // namespace Licasa
