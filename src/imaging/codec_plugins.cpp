#include "imaging/codec_plugins.h"
#include "codec_plugin_paths.h"

#include <QCoreApplication>
#include <QDir>

void Licasa::initializeImagePlugins()
{
    const QDir executable(QCoreApplication::applicationDirPath());
    // Both paths are relative to this executable so an installed package works
    // without a developer's QT_PLUGIN_PATH or build directory.
    QCoreApplication::addLibraryPath(
        executable.filePath(QStringLiteral(LICASA_INSTALL_PLUGIN_RELATIVE)));
    QCoreApplication::addLibraryPath(executable.filePath(QStringLiteral("plugins")));
}
