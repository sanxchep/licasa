#pragma once

#include <QDate>
#include <QObject>
#include <QSettings>
#include <QStringList>
#include <QTranslator>
#include <QVariantList>
#include <memory>

namespace Licasa {

class LanguageManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString selectedLanguage READ selectedLanguage NOTIFY languageChanged)
    Q_PROPERTY(QString effectiveLanguage READ effectiveLanguage NOTIFY languageChanged)
    Q_PROPERTY(QVariantList languages READ languages NOTIFY languageChanged)
    Q_PROPERTY(QString releaseDate READ releaseDate NOTIFY languageChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)

  public:
    explicit LanguageManager(QObject* parent = nullptr);
    LanguageManager(const QString& settingsFile, const QString& catalogDirectory,
                    const QStringList& systemUiLanguages, QObject* parent = nullptr);
    ~LanguageManager() override;

    QString selectedLanguage() const;
    QString effectiveLanguage() const;
    QVariantList languages() const;
    QString releaseDate() const;
    QString errorMessage() const;
    void setReleaseDate(const QDate& date);
    Q_INVOKABLE bool selectLanguage(const QString& language);

  signals:
    void languageChanged();
    void errorMessageChanged();

  private:
    QString resolve(const QString& language) const;
    bool prepare(const QString& language, std::unique_ptr<QTranslator>& translator) const;
    void apply(const QString& selected, std::unique_ptr<QTranslator> translator);
    bool fail(const QString& message);

    std::unique_ptr<QSettings> settings_;
    QString catalogDirectory_;
    QStringList systemUiLanguages_;
    QString selectedLanguage_ = QStringLiteral("system");
    QString effectiveLanguage_ = QStringLiteral("en");
    QString errorMessage_;
    QDate releaseDate_;
    std::unique_ptr<QTranslator> translator_;
};

} // namespace Licasa
