#pragma once

#include "config/types.h"
#include "kdl/kdl.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <expected>

namespace Konveyor::Config
{

struct LoadError
{
    QString message;
    Kdl::Location location;
    QString sourceLine;
    QStringList files;

    QString toString() const;
};

struct Diagnostic
{
    QString message;
    Kdl::Location location;
    QString sourceLine;

    QString toString() const;
};

struct LoadOptions
{
    bool recover = false;
};

struct LoadResult
{
    Config config;
    QStringList files;
    QStringList warnings;
    QList<Diagnostic> errors;
};

std::expected<LoadResult, LoadError> loadString(const QString &text, const QString &fileName, LoadOptions options = {});
std::expected<LoadResult, LoadError> loadFile(const QString &path, LoadOptions options = {});
std::expected<LoadResult, LoadError> loadSnapshot(
    const QString &path, const QHash<QString, QString> &files, LoadOptions options = {});
QString configPath();
Config defaultConfig();
Layout mergedLayout(Layout base, const LayoutPart &part);
quint32 keysymFromName(const QString &name);
QString bindKeyLabel(const Bind &bind);
bool bindActsOnPointerOutput(const Bind &bind, const Input &input);

}
