#include "plugin/configmanager.h"

#include "config/forceresizable.h"
#include "config/loader.h"
#include "config/log.h"
#include "document/configmigration.h"
#include "plugin/notifications.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace Konveyor
{

namespace
{

constexpr int reloadDebounceMs = 5;
constexpr QLatin1StringView forceResizableFileName("force-resizable.kdl");

QString bundledDefaultConfig()
{
    return QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("konveyor/default-config.kdl"));
}

QString migrationsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/konveyor/config-migrations");
}

std::expected<QString, QString> readText(const QString &path, bool allowMissing);

QString lastGoodPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/konveyor/config-last-good.json");
}

QString lastGoodJson(const QString &path, const QHash<QString, QString> &files)
{
    QJsonObject texts;
    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        texts.insert(it.key(), it.value());
    }
    QJsonObject object;
    object.insert(QStringLiteral("version"), 1);
    object.insert(QStringLiteral("path"), QFileInfo(path).absoluteFilePath());
    object.insert(QStringLiteral("files"), texts);
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

std::optional<QHash<QString, QString>> readLastGood(const QString &target)
{
    const auto serialized = readText(lastGoodPath(), true);
    if (!serialized || serialized->isEmpty()) {
        return std::nullopt;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(serialized->toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qCInfo(lcKonveyor).noquote() << "konveyor: could not read" << lastGoodPath() << error.errorString();
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    const QString absolute = QFileInfo(target).absoluteFilePath();
    if (object.value(QStringLiteral("version")).toInt() != 1 || object.value(QStringLiteral("path")).toString() != absolute) {
        return std::nullopt;
    }
    const QJsonObject files = object.value(QStringLiteral("files")).toObject();
    QHash<QString, QString> texts;
    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        if (it.value().isString()) {
            texts.insert(it.key(), it.value().toString());
        }
    }
    if (!texts.contains(absolute)) {
        return std::nullopt;
    }
    return texts;
}

std::expected<QString, QString> readText(const QString &path, bool allowMissing)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (allowMissing && !file.exists()) {
            return QString();
        }
        return std::unexpected(QStringLiteral("Could not read %1: %2").arg(path, file.errorString()));
    }
    return QString::fromUtf8(file.readAll());
}

std::expected<void, QString> writeTexts(const QList<std::pair<QString, QString>> &files)
{
    std::vector<std::unique_ptr<QSaveFile>> pending;
    for (const auto &[path, text] : files) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        auto file = std::make_unique<QSaveFile>(path);
        if (!file->open(QIODevice::WriteOnly | QIODevice::Text) || file->write(text.toUtf8()) < 0) {
            return std::unexpected(QStringLiteral("Could not write %1: %2").arg(path, file->errorString()));
        }
        pending.push_back(std::move(file));
    }
    for (const std::unique_ptr<QSaveFile> &file : pending) {
        if (!file->commit()) {
            return std::unexpected(QStringLiteral("Could not write %1: %2").arg(file->fileName(), file->errorString()));
        }
    }
    return {};
}

}

ConfigManager::ConfigManager(QObject *parent)
    : QObject(parent)
    , m_config(Config::defaultConfig())
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(reloadDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this, [this] { load(); });
    const auto schedule = [this] { m_debounce.start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, schedule);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, schedule);
}

void ConfigManager::start()
{
    m_path = Config::configPath();
    ensureConfigFileExists();
    migrate();
    load();
}

QString ConfigManager::load(const QString &path)
{
    const QString target = path.isEmpty() ? m_path : path;
    auto result = Config::loadFile(target, {.recover = true});
    if (!result) {
        const QString message = result.error().toString();
        qCInfo(lcKonveyor).noquote() << "konveyor: config" << target << "failed to load:" << message;
        if (!m_loadedOnce) {
            if (const auto files = readLastGood(target)) {
                if (auto restored = Config::loadSnapshot(target, *files, {.recover = true})) {
                    m_loadedOnce = true;
                    m_path = target;
                    m_config = std::move(restored->config);
                    QStringList watched = result.error().files;
                    watched.append(restored->files);
                    watch(watched);
                    notifyWarnings(restored->warnings);
                    qCInfo(lcKonveyor).noquote() << "konveyor: using the last config that loaded for" << target;
                    Q_EMIT configChanged(m_config);
                    notifyFailure(message + QStringLiteral("\nUsing the last config that loaded."));
                    Q_EMIT configLoaded(true);
                    return message;
                }
            }
        }
        notifyFailure(message);
        if (target == m_path) {
            watch(result.error().files);
        }
        if (!m_loadedOnce) {
            Q_EMIT configChanged(m_config);
        }
        Q_EMIT configLoaded(true);
        return message;
    }
    m_loadedOnce = true;
    qCInfo(lcKonveyor).noquote() << "konveyor: config loaded from" << target << "files:" << result->files.join(QStringLiteral(", "));
    m_path = target;
    watch(result->files);
    notifyWarnings(result->warnings);
    m_config = std::move(result->config);
    rememberLastGood(target, result->files);
    Q_EMIT configChanged(m_config);
    Q_EMIT configLoaded(false);
    if (result->errors.isEmpty()) {
        return QString();
    }
    QStringList messages;
    messages.reserve(result->errors.size());
    for (const Config::Diagnostic &error : std::as_const(result->errors)) {
        messages.append(error.toString());
    }
    const QString message = messages.join(QLatin1Char('\n'));
    qCWarning(lcKonveyor).noquote() << "konveyor: config entries ignored in" << target << ":\n" << message;
    notifyRecovered(message);
    return message;
}

const Config::Config &ConfigManager::config() const
{
    return m_config;
}

std::expected<void, QString> ConfigManager::setForceResizable(const QString &appId, bool enabled)
{
    const QString overridePath = QDir(QFileInfo(m_path).absolutePath()).filePath(forceResizableFileName);
    const auto overrideText = readText(overridePath, true);
    if (!overrideText) {
        return std::unexpected(overrideText.error());
    }
    const auto updatedOverride = Config::setForceResizableRule(*overrideText, overridePath, appId, enabled);
    if (!updatedOverride) {
        return std::unexpected(updatedOverride.error());
    }
    const auto mainText = readText(m_path, false);
    if (!mainText) {
        return std::unexpected(mainText.error());
    }
    const auto updatedMain = Config::ensureForceResizableInclude(*mainText, m_path);
    if (!updatedMain) {
        return std::unexpected(updatedMain.error());
    }
    QList<std::pair<QString, QString>> files {{overridePath, *updatedOverride}};
    if (*updatedMain != *mainText) {
        files.append({m_path, *updatedMain});
    }
    if (const auto written = writeTexts(files); !written) {
        return written;
    }
    const QString error = load();
    if (!error.isEmpty()) {
        return std::unexpected(error);
    }
    return {};
}

std::expected<void, QString> ConfigManager::rewrite(const std::function<std::expected<QString, QString>(const QString &)> &edit)
{
    const auto text = readText(m_path, false);
    if (!text) {
        return std::unexpected(text.error());
    }
    const auto edited = edit(*text);
    if (!edited) {
        return std::unexpected(edited.error());
    }
    if (*edited == *text) {
        return {};
    }
    if (const auto loaded = Config::loadString(*edited, m_path); !loaded) {
        return std::unexpected(loaded.error().toString());
    }
    return writeTexts({{m_path, *edited}});
}

std::expected<QString, QString> ConfigManager::text() const
{
    return readText(m_path, false);
}

void ConfigManager::migrate()
{
    const auto applied = readText(migrationsPath(), true);
    const auto defaults = readText(bundledDefaultConfig(), false);
    if (!applied || !defaults) {
        return;
    }
    QStringList done = applied->split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const Settings::ConfigMigration &migration : Settings::configMigrations()) {
        if (done.contains(migration.id)) {
            continue;
        }
        if (!Settings::shipsAny(*defaults, migration.shippedPaths)) {
            qCInfo(lcKonveyor).noquote() << "konveyor: config migration" << migration.id << "waits for the defaults that ship it";
            continue;
        }
        const auto migrated = rewrite([&](const QString &text) { return Settings::applyMigration(migration, text, m_path, *defaults); });
        if (!migrated) {
            qCInfo(lcKonveyor).noquote() << "konveyor: config migration" << migration.id << "skipped:" << migrated.error();
            return;
        }
        qCInfo(lcKonveyor).noquote() << "konveyor: config migration" << migration.id << "applied to" << m_path;
        done.append(migration.id);
        if (const auto written = writeTexts({{migrationsPath(), done.join(QLatin1Char('\n')) + QLatin1Char('\n')}}); !written) {
            qCInfo(lcKonveyor).noquote() << "konveyor:" << written.error();
            return;
        }
    }
}

void ConfigManager::ensureConfigFileExists() const
{
    if (QFileInfo::exists(m_path)) {
        return;
    }
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    const QString source = bundledDefaultConfig();
    if (source.isEmpty() || !QFile::copy(source, m_path)) {
        notifyFailure(QStringLiteral("Could not create %1 from the bundled default config.").arg(m_path));
        return;
    }
    QFile::setPermissions(m_path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
}

void ConfigManager::watch(const QStringList &files)
{
    QStringList wanted;
    for (const QString &file : files) {
        wanted.append(QFileInfo(file).absolutePath());
    }
    std::ranges::copy_if(files, std::back_inserter(wanted), [](const QString &file) { return QFileInfo::exists(file); });
    wanted.removeDuplicates();
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    QStringList unwanted;
    std::ranges::copy_if(watched, std::back_inserter(unwanted), [&wanted](const QString &path) { return !wanted.contains(path); });
    QStringList missing;
    std::ranges::copy_if(wanted, std::back_inserter(missing), [&watched](const QString &path) { return !watched.contains(path); });
    if (!unwanted.isEmpty()) {
        m_watcher.removePaths(unwanted);
    }
    if (!missing.isEmpty()) {
        m_watcher.addPaths(missing);
    }
}

void ConfigManager::rememberLastGood(const QString &path, const QStringList &files)
{
    QHash<QString, QString> texts;
    for (const QString &file : files) {
        const auto text = readText(file, false);
        if (text) {
            texts.insert(QFileInfo(file).absoluteFilePath(), *text);
        }
    }
    if (texts.isEmpty()) {
        return;
    }
    const QString serialized = lastGoodJson(path, texts);
    if (m_lastGoodSerialized.isEmpty()) {
        m_lastGoodSerialized = readText(lastGoodPath(), true).value_or(QString());
    }
    if (serialized == m_lastGoodSerialized && QFile::exists(lastGoodPath())) {
        return;
    }
    QDir().mkpath(QFileInfo(lastGoodPath()).absolutePath());
    QSaveFile file(lastGoodPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(serialized.toUtf8()) < 0 || !file.commit()) {
        qCInfo(lcKonveyor).noquote() << "konveyor: could not write" << lastGoodPath() << file.errorString();
        return;
    }
    QFile::setPermissions(lastGoodPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    m_lastGoodSerialized = serialized;
}

void ConfigManager::notifyFailure(const QString &message) const
{
    if (m_config.configNotificationDisableFailed) {
        return;
    }
    sendNotification(QStringLiteral("Konveyor: failed to load config"), message);
}

void ConfigManager::notifyRecovered(const QString &message) const
{
    if (m_config.configNotificationDisableFailed) {
        return;
    }
    sendNotification(QStringLiteral("Konveyor: config entries ignored"), message);
}

void ConfigManager::notifyWarnings(const QStringList &warnings) const
{
    if (!warnings.isEmpty()) {
        qInfo().noquote() << "konveyor: config warnings:\n" << warnings.join(QLatin1Char('\n'));
    }
}

}
