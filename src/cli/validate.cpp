#include "validate.h"

#include "config/loader.h"

namespace Konveyor::Cli
{

int runValidate(const QString &path, QTextStream &out, QTextStream &err)
{
    const auto result = Config::loadFile(path, {.recover = true});
    if (!result) {
        err << "error: " << result.error().toString() << "\n";
        err.flush();
        return 1;
    }
    for (const QString &warning : result->warnings) {
        err << "warning: " << warning << "\n";
    }
    for (const Config::Diagnostic &error : result->errors) {
        err << "error: " << error.toString() << "\n";
    }
    err.flush();
    if (!result->errors.isEmpty()) {
        return 1;
    }
    out << "Config is valid: " << path << "\n";
    out.flush();
    return 0;
}

}
