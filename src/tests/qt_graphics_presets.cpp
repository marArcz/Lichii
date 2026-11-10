// SPDX-License-Identifier: GPL-3.0-or-later
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QInputDialog>
#include <QTimer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "yuzu/configuration/graphics_presets.h"
#include "yuzu/configuration/shared_widget.h"

class GraphicsPresetTests : public QObject {
    Q_OBJECT
private slots:
    void draftAndOverrides() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto original = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
        Common::FS::SetEdenPath(Common::FS::EdenPath::ConfigDir, directory.path().toStdString());
        for (const bool global : {true, false}) {
            Settings::SetConfiguringGlobal(global);
            auto& resolution = Settings::values.resolution_setup;
            resolution.SetGlobal(true);
            resolution.SetValue(Settings::ResolutionSetup::Res4X);
            QWidget owner;
            QVBoxLayout layout(&owner);
            ConfigurationShared::Builder builder(&owner, true);
            std::vector<std::function<void(bool)>> apply;
            auto* field = builder.BuildWidget(&resolution, apply);
            layout.addWidget(field);
            InstallGraphicsPresets(owner, owner, false, 0);
            auto* selector = owner.findChild<QComboBox*>();
            // The settings combobox predates the preset selector.
            for (auto* box : owner.findChildren<QComboBox*>()) {
                if (box->findText(QStringLiteral("Performance")) >= 0) selector = box;
            }
            QVERIFY(selector);
            QTimer::singleShot(0, [] {
                auto* prompt = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
                if (prompt) { prompt->setTextValue(QStringLiteral("My laptop")); prompt->accept(); }
            });
            for (auto* button : owner.findChildren<QPushButton*>())
                if (button->text() == QStringLiteral("Save As…")) button->click();
            QFile saved(directory.filePath(QStringLiteral("graphics-presets.json")));
            QVERIFY(saved.open(QIODevice::ReadOnly));
            const auto catalog = QJsonDocument::fromJson(saved.readAll()).object();
            const auto presets = catalog.value(QStringLiteral("presets")).toArray();
            QVERIFY(!presets.isEmpty());
            QCOMPARE(presets.first().toObject().value(QStringLiteral("settings")).toObject()
                         .value(QStringLiteral("resolution_setup")).toString(), QStringLiteral("Res4X"));
            saved.close();
            // Remove the saved entry between global/per-game cases to avoid replacement prompts.
            QVERIFY(saved.remove());
            QVERIFY(selector->findText(QStringLiteral("Ocarina — 4 GB laptop")) < 0);
            QVERIFY(field->SetDraftValue(std::to_string(static_cast<u32>(Settings::ResolutionSetup::Res3_4X))));
            QCOMPARE(resolution.GetValue(true), Settings::ResolutionSetup::Res4X);
            QCOMPARE(resolution.UsingGlobal(), true);
            if (!global) QVERIFY(field->restore_button->isEnabled());
            for (const auto& commit : apply) commit(false);
            QCOMPARE(resolution.GetValue(), Settings::ResolutionSetup::Res3_4X);
            if (!global) {
                QCOMPARE(resolution.GetValue(true), Settings::ResolutionSetup::Res4X);
                QCOMPARE(resolution.UsingGlobal(), false);
            }
            resolution.SetGlobal(true);
        }
        Settings::SetConfiguringGlobal(true);
        Common::FS::SetEdenPath(Common::FS::EdenPath::ConfigDir, original);
    }
    void runningGame() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto original = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
        Common::FS::SetEdenPath(Common::FS::EdenPath::ConfigDir, directory.path().toStdString());
        QWidget owner;
        QVBoxLayout layout(&owner);
        InstallGraphicsPresets(owner, owner, true, 0x0400486024DEC000ULL);
        bool found = false;
        for (auto* button : owner.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Apply Preset")) {
                found = true; QVERIFY(!button->isEnabled());
            }
        }
        QVERIFY(found);
        QVERIFY(owner.findChild<QComboBox*>()->findText(QStringLiteral("Ocarina — 4 GB laptop")) >= 0);
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("graphics-presets.json"))));
        Common::FS::SetEdenPath(Common::FS::EdenPath::ConfigDir, original);
    }
};
QTEST_MAIN(GraphicsPresetTests)
#include "qt_graphics_presets.moc"
