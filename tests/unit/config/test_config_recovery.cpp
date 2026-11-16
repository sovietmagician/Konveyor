#include "configtesthelpers.h"

#include <QTest>

using namespace Konveyor::Config;
using namespace Konveyor::Config::Testing;

namespace
{

QString diagnosticMessage(const Diagnostic &diagnostic)
{
    return QStringLiteral("%1:%2: %3").arg(diagnostic.location.line).arg(diagnostic.location.column).arg(diagnostic.message);
}

}

class TestConfigRecovery : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void dropsOnlyTheBadSection();
    void keepsGoodBindsAndDropsBadOnes();
    void keepsAWindowRulesValidChildren();
    void keepsABorderStylesValidLayers();
    void skipsAMissingInclude();
    void keepsSyntaxErrorsInIncludesFatal();
    void keepsTheFirstDuplicateSection();
    void reportsEveryBadEntry();
    void dropsTheLaterModKeyConflict();
    void strictModeStillFails();
    void sourceLinePointsAtTheBadEntry();

private:
    ConfigDir m_dir;
};

void TestConfigRecovery::init()
{
    m_dir.clear();
}

void TestConfigRecovery::dropsOnlyTheBadSection()
{
    const LoadResult result = mustRecover(QStringLiteral(R"(
layout {
    gaps "wide"
}
hide-desktop-widgets
)"));
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(diagnosticMessage(result.errors.first()), QStringLiteral("3:10: unsupported value, only numbers are recognized"));
    QCOMPARE(result.config.layout.gaps, 16.0);
    QCOMPARE(result.config.hideDesktopWidgets, true);
}

void TestConfigRecovery::keepsGoodBindsAndDropsBadOnes()
{
    const LoadResult result = mustRecover(QStringLiteral(R"(
binds {
    Mod+A { close-window; }
    Mod+B { no-such-action; }
    Mod+C { toggle-overview; }
}
)"));
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.config.binds.size(), 2);
    QCOMPARE(result.config.binds.at(0).keyText, QStringLiteral("A"));
    QCOMPARE(result.config.binds.at(1).keyText, QStringLiteral("C"));
    QCOMPARE(result.errors.first().message, QStringLiteral("unknown action `no-such-action`"));
}

void TestConfigRecovery::keepsAWindowRulesValidChildren()
{
    const LoadResult result = mustRecover(QStringLiteral(R"(
window-rule {
    match app-id="^kitty$"
    opacity 5
    open-maximized true
}
)"));
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.config.windowRules.size(), 1);
    const WindowRule &rule = result.config.windowRules.first();
    QCOMPARE(rule.matches.size(), 1);
    QCOMPARE(rule.matches.first().appId->pattern(), QStringLiteral("^kitty$"));
    QCOMPARE(rule.opacity, std::optional<double>());
    QCOMPARE(rule.openMaximized, std::optional<bool>(true));
    QCOMPARE(result.errors.first().message, QStringLiteral("value must be between 0 and 1"));
}

void TestConfigRecovery::keepsABorderStylesValidLayers()
{
    const LoadResult result = mustRecover(QStringLiteral(R"(
border-style "Mine" {
    layer "gradient" {
        colors "#ff0000"
    }
    layer "bogus" {
    }
}
)"));
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.config.borderStyles.size(), 1);
    const BorderStyle &style = result.config.borderStyles.first();
    QCOMPARE(style.name, QStringLiteral("Mine"));
    QCOMPARE(style.layers.size(), 1);
    QCOMPARE(style.layers.first().kind, BorderLayerKind::Gradient);
    QVERIFY(result.errors.first().message.startsWith(QStringLiteral("expected one of ")));
}

void TestConfigRecovery::skipsAMissingInclude()
{
    m_dir.write(QStringLiteral("config.kdl"), QStringLiteral("include \"absent.kdl\"\nlayout {\n    gaps 4\n}\n"));
    const LoadResult result = m_dir.recover();
    QCOMPARE(result.errors.size(), 1);
    QVERIFY(result.errors.first().message.contains(QStringLiteral("failed to read included config")));
    QCOMPARE(result.config.layout.gaps, 4.0);
}

void TestConfigRecovery::keepsSyntaxErrorsInIncludesFatal()
{
    m_dir.write(QStringLiteral("config.kdl"), QStringLiteral("include \"broken.kdl\"\nlayout {\n    gaps 4\n}\n"));
    m_dir.write(QStringLiteral("broken.kdl"), QStringLiteral("layout {\n"));

    const auto strict = m_dir.tryLoad();
    QVERIFY(!strict.has_value());
    QCOMPARE(strict.error().location.file, QStringLiteral("broken.kdl"));

    const auto recover = m_dir.tryRecover();
    QVERIFY(!recover.has_value());
    QCOMPARE(recover.error().location.file, QStringLiteral("broken.kdl"));
    QCOMPARE(recover.error().message, strict.error().message);
}

void TestConfigRecovery::keepsTheFirstDuplicateSection()
{
    const LoadResult result = mustRecover(QStringLiteral("layout {\n    gaps 4\n}\nlayout {\n    gaps 9\n}\n"));
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.errors.first().message, QStringLiteral("duplicate node `layout`, single node expected"));
    QCOMPARE(result.config.layout.gaps, 4.0);
}

void TestConfigRecovery::reportsEveryBadEntry()
{
    const LoadResult result = mustRecover(QStringLiteral(R"(
layout {
    gaps "wide"
}
binds {
    Mod+A { close-window; }
    Mod+B { no-such-action; }
}
window-rule {
    match app-id="^a$"
    opacity 5
}
)"));
    QCOMPARE(result.errors.size(), 3);
    QCOMPARE(result.config.layout.gaps, 16.0);
    QCOMPARE(result.config.binds.size(), 1);
    QCOMPARE(result.config.windowRules.size(), 1);
    QCOMPARE(result.config.windowRules.first().matches.size(), 1);
}

void TestConfigRecovery::dropsTheLaterModKeyConflict()
{
    const LoadResult result = mustRecover(QStringLiteral(R"(
input {
    mod-key "Super"
}
binds {
    Super+A { close-window; }
    Mod+A { toggle-overview; }
}
)"));
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.errors.first().message,
        QStringLiteral("keybind `Mod+A` is the same as `Super+A` while the Mod key is Super"));
    QCOMPARE(result.config.binds.size(), 1);
    QCOMPARE(result.config.binds.first().keyText, QStringLiteral("A"));
    QCOMPARE(result.config.binds.first().action.name, QStringLiteral("close-window"));
}

void TestConfigRecovery::strictModeStillFails()
{
    const QString text = QStringLiteral("binds {\n    Mod+A { close-window; }\n    Mod+B { no-such-action; }\n}\n");
    QVERIFY(!mustRecover(text).errors.isEmpty());
    QCOMPARE(failure(text), QStringLiteral("3:13: unknown action `no-such-action`"));
}

void TestConfigRecovery::sourceLinePointsAtTheBadEntry()
{
    const LoadResult result = mustRecover(QStringLiteral("layout {\n    gaps \"wide\"\n}\n"));
    QCOMPARE(result.errors.size(), 1);
    const Diagnostic &diagnostic = result.errors.first();
    QCOMPARE(diagnostic.location.file, QStringLiteral("config.kdl"));
    QCOMPARE(diagnostic.location.line, 2);
    QCOMPARE(diagnostic.sourceLine, QStringLiteral("    gaps \"wide\""));
    QVERIFY(diagnostic.toString().startsWith(QStringLiteral("config.kdl:2:10: ")));
    QVERIFY(diagnostic.toString().endsWith(QStringLiteral("\n    gaps \"wide\"")));
}

QTEST_MAIN(TestConfigRecovery)
#include "test_config_recovery.moc"
