#include "app/language_manager.h"

#include <QCoreApplication>
#include <QLocale>
#include <QResource>

static void ensureTranslationResources()
{
    // Referencing the resource explicitly keeps it linked from the static
    // services library into the viewer, diagnostics and localization tests.
    static const bool initialized = [] {
        Q_INIT_RESOURCE(licasa_translations);
        return true;
    }();
    Q_UNUSED(initialized);
}

namespace Licasa {
namespace {
constexpr auto languageKey = "viewer/language";
bool supported(const QString& language)
{
    return language == QStringLiteral("system") || language == QStringLiteral("en") ||
           language == QStringLiteral("sv");
}
} // namespace

LanguageManager::LanguageManager(QObject* parent)
    : LanguageManager({}, QStringLiteral(":/i18n"), QLocale::system().uiLanguages(), parent)
{}

LanguageManager::LanguageManager(const QString& settingsFile, const QString& catalogDirectory,
                                 const QStringList& systemUiLanguages, QObject* parent)
    : QObject(parent),
      settings_(settingsFile.isEmpty()
                    ? std::make_unique<QSettings>()
                    : std::make_unique<QSettings>(settingsFile, QSettings::IniFormat)),
      catalogDirectory_(catalogDirectory), systemUiLanguages_(systemUiLanguages)
{
    ensureTranslationResources();
    QString selected =
        settings_->value(QLatin1String(languageKey), QStringLiteral("system")).toString();
    if (!supported(selected)) {
        selected = QStringLiteral("system");
    }
    std::unique_ptr<QTranslator> translator;
    if (!prepare(selected, translator)) {
        translator.reset();
        selected = QStringLiteral("en");
        errorMessage_ = tr("The selected language is unavailable. Using English.");
    }
    apply(selected, std::move(translator));
}

LanguageManager::~LanguageManager()
{
    if (translator_) {
        QCoreApplication::removeTranslator(translator_.get());
    }
}

QString LanguageManager::selectedLanguage() const { return selectedLanguage_; }
QString LanguageManager::effectiveLanguage() const { return effectiveLanguage_; }
QString LanguageManager::errorMessage() const { return errorMessage_; }

QVariantList LanguageManager::languages() const
{
    // Autonyms keep explicit languages recognizable after an accidental switch.
    return {QVariantMap{{QStringLiteral("value"), QStringLiteral("system")},
                        {QStringLiteral("text"), tr("System default")}},
            QVariantMap{{QStringLiteral("value"), QStringLiteral("en")},
                        {QStringLiteral("text"), QStringLiteral("English")}},
            QVariantMap{{QStringLiteral("value"), QStringLiteral("sv")},
                        {QStringLiteral("text"), QString::fromUtf8("Svenska")}}};
}

QString LanguageManager::releaseDate() const
{
    const QLocale locale(effectiveLanguage_ == QStringLiteral("sv") ? QLocale::Swedish
                                                                    : QLocale::English);
    return locale.toString(releaseDate_, QStringLiteral("d MMMM yyyy"));
}

void LanguageManager::setReleaseDate(const QDate& date)
{
    if (releaseDate_ != date) {
        releaseDate_ = date;
        emit languageChanged();
    }
}

QString LanguageManager::resolve(const QString& language) const
{
    if (language != QStringLiteral("system")) {
        return language;
    }
    for (const QString& candidate : systemUiLanguages_) {
        const QString base =
            candidate.section(QLatin1Char('-'), 0, 0).section(QLatin1Char('_'), 0, 0).toLower();
        if (base == QStringLiteral("sv") || base == QStringLiteral("en")) {
            return base;
        }
    }
    return QStringLiteral("en");
}

bool LanguageManager::prepare(const QString& language,
                              std::unique_ptr<QTranslator>& translator) const
{
    if (resolve(language) == QStringLiteral("en")) {
        return true;
    }
    translator = std::make_unique<QTranslator>();
    return translator->load(QStringLiteral("licasa_sv"), catalogDirectory_) &&
           !translator->isEmpty();
}

void LanguageManager::apply(const QString& selected, std::unique_ptr<QTranslator> translator)
{
    if (translator_) {
        QCoreApplication::removeTranslator(translator_.get());
    }
    translator_ = std::move(translator);
    if (translator_) {
        QCoreApplication::installTranslator(translator_.get());
    }
    selectedLanguage_ = selected;
    effectiveLanguage_ = resolve(selected);
    emit languageChanged();
}

bool LanguageManager::fail(const QString& message)
{
    errorMessage_ = message;
    emit errorMessageChanged();
    return false;
}

bool LanguageManager::selectLanguage(const QString& language)
{
    if (!supported(language)) {
        return fail(tr("This language is not available."));
    }
    if (language == selectedLanguage_ &&
        settings_->value(QLatin1String(languageKey)).toString() == language) {
        errorMessage_.clear();
        emit errorMessageChanged();
        return true;
    }
    std::unique_ptr<QTranslator> translator;
    if (!prepare(language, translator)) {
        return fail(tr("The language could not be loaded. Your current language is unchanged."));
    }
    const QVariant previous = settings_->value(QLatin1String(languageKey));
    settings_->setValue(QLatin1String(languageKey), language);
    settings_->sync();
    if (settings_->status() != QSettings::NoError) {
        if (previous.isValid()) {
            settings_->setValue(QLatin1String(languageKey), previous);
        } else {
            settings_->remove(QLatin1String(languageKey));
        }
        return fail(
            tr("The language preference could not be saved. Your current language is unchanged."));
    }
    errorMessage_.clear();
    apply(language, std::move(translator));
    emit errorMessageChanged();
    return true;
}

} // namespace Licasa
