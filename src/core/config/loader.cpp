#include "config/loader.h"

#include "config/sections.h"
#include "kdl/characters.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>

namespace Konveyor::Config
{

namespace
{

Paint solidPaint(int red, int green, int blue, int alpha)
{
    return Paint {ColorSource::Explicit, QColor(red, green, blue, alpha), std::nullopt};
}

QString formatted(const QString &message, const Kdl::Location &location, const QString &sourceLine)
{
    QString result = QStringLiteral("%1:%2:%3: %4").arg(location.file).arg(location.line).arg(location.column).arg(message);
    if (!sourceLine.isEmpty()) {
        result += QLatin1Char('\n');
        result += sourceLine;
    }
    return result;
}

void warnAboutHiddenWindowSwipes(LoadContext &context)
{
    const QList<std::pair<QString, const MultiTouch *>> devices {{QStringLiteral("touchpad"), &context.config.gestures.touchpad},
        {QStringLiteral("touchscreen"), &context.config.gestures.touchscreen}};
    for (const auto &[name, touch] : devices) {
        const bool rowSwipes = touch->horizontalSwipe != HorizontalSwipe::Off || touch->verticalSwipe != VerticalSwipe::Off;
        const bool windowSwipes
            = touch->windowHorizontalSwipe != WindowHorizontalSwipe::Off || touch->windowVerticalSwipe != WindowVerticalSwipe::Off;
        if (touch->enabled && rowSwipes && windowSwipes && touch->swipeFingers == touch->windowSwipeFingers) {
            context.warnings.append(QStringLiteral("%1 swipe-fingers and window-swipe-fingers are both %2, so the window swipes never "
                                                   "start; give them different finger counts")
                    .arg(name)
                    .arg(touch->swipeFingers));
        }
    }
}

std::expected<LoadResult, LoadError> runLoad(LoadContext &context, const QString &text, const QString &name, const QString &baseDir,
    LoadOptions options, const QHash<QString, QString> *snapshot = nullptr)
{
    context.config = defaultConfig();
    context.rootDir = baseDir;
    context.recover = options.recover;
    context.snapshot = snapshot;
    context.texts.insert(name, text);
    const auto document = Kdl::parse(text, name);
    if (!document) {
        const Kdl::ParseError &error = document.error();
        return std::unexpected(LoadError {error.message, error.location, sourceLineOf(context, error.location), context.files});
    }
    try {
        processDocument(context, *document, baseDir, QStringList {QDir(baseDir).filePath(name)});
        resolveBinds(context);
        warnAboutHiddenWindowSwipes(context);
        warnAboutUnknownBorderStyles(context);
    } catch (const FatalDecodeError &error) {
        return std::unexpected(LoadError {error.message, error.location, sourceLineOf(context, error.location), context.files});
    } catch (const DecodeError &error) {
        return std::unexpected(LoadError {error.message, error.location, sourceLineOf(context, error.location), context.files});
    }
    return LoadResult {context.config, context.files, context.warnings, context.errors};
}

}

QString sourceLineOf(const LoadContext &context, const Kdl::Location &location)
{
    return Kdl::splitLines(context.texts.value(location.file)).value(location.line - 1);
}

void reportRecoveredAt(LoadContext &context, const Kdl::Location &location, const QString &message)
{
    context.errors.append(Diagnostic {message, location, sourceLineOf(context, location)});
}

void reportRecovered(LoadContext &context, const DecodeError &error)
{
    reportRecoveredAt(context, error.location, error.message);
}

QString LoadError::toString() const
{
    return formatted(message, location, sourceLine);
}

QString Diagnostic::toString() const
{
    return formatted(message, location, sourceLine);
}

Config defaultConfig()
{
    Config config;
    config.layout.focusRing.enabled = true;
    config.layout.focusRing.width = 2;
    config.layout.focusRing.active = solidPaint(127, 200, 255, 255);
    config.layout.focusRing.urgent = solidPaint(155, 0, 0, 255);
    config.layout.border.enabled = false;
    config.layout.border.active = solidPaint(255, 200, 127, 255);
    config.layout.border.inactive = solidPaint(80, 80, 80, 255);
    config.layout.border.urgent = solidPaint(155, 0, 0, 255);
    config.layout.tabIndicator.inactive = solidPaint(80, 80, 80, 255);
    config.layout.insertHint.paint = solidPaint(127, 200, 255, 128);
    config.layout.presetColumnWidths = defaultColumnWidthPresets();
    config.layout.presetWindowHeights = defaultWindowHeightPresets();
    config.animations = defaultAnimations();
    return config;
}

std::expected<LoadResult, LoadError> loadString(const QString &text, const QString &fileName, LoadOptions options)
{
    const QFileInfo info(fileName);
    const QString name = info.fileName().isEmpty() ? QStringLiteral("config.kdl") : info.fileName();
    LoadContext context;
    context.files.append(fileName);
    return runLoad(context, text, name, info.path(), options);
}

std::expected<LoadResult, LoadError> loadFile(const QString &path, LoadOptions options)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return std::unexpected(LoadError {QStringLiteral("error reading %1: %2").arg(path, file.errorString()), Kdl::Location {path, 0, 0},
            QString(), QStringList {path}});
    }
    const QString text = QString::fromUtf8(file.readAll());
    const QFileInfo info(path);
    LoadContext context;
    context.files.append(info.absoluteFilePath());
    return runLoad(context, text, info.fileName(), info.absolutePath(), options);
}

std::expected<LoadResult, LoadError> loadSnapshot(const QString &path, const QHash<QString, QString> &files, LoadOptions options)
{
    const QFileInfo info(path);
    const QString absolute = info.absoluteFilePath();
    const auto text = files.constFind(absolute);
    if (text == files.constEnd()) {
        return std::unexpected(LoadError {QStringLiteral("the saved config does not contain %1").arg(absolute),
            Kdl::Location {absolute, 0, 0}, QString(), QStringList {absolute}});
    }
    LoadContext context;
    context.files.append(absolute);
    return runLoad(context, *text, info.fileName(), info.absolutePath(), options, &files);
}

QString configPath()
{
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QString explicitPath = environment.value(QStringLiteral("KONVEYOR_CONFIG"));
    if (!explicitPath.isEmpty()) {
        return explicitPath;
    }
    const QString configHome = environment.value(QStringLiteral("XDG_CONFIG_HOME"));
    const QString base = configHome.isEmpty() ? QDir(QDir::homePath()).filePath(QStringLiteral(".config")) : configHome;
    return QDir(base).filePath(QStringLiteral("konveyor/config.kdl"));
}

}
