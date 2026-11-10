// SPDX-License-Identifier: GPL-3.0-or-later
#include "yuzu/configuration/graphics_presets.h"

#include <functional>
#include <type_traits>
#include <vector>
#include <QCoreApplication>
#include <QEvent>
#include <QFileInfo>
#include <map>
#include <optional>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QUuid>
#include <QVBoxLayout>
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "yuzu/configuration/shared_widget.h"

namespace {
constexpr qint64 MaxFileSize = 1024 * 1024;
constexpr u64 Ocarina = 0x0400486024DEC000ULL;
using Fields = std::map<QString, ConfigurationShared::Widget*>;
struct Descriptor {
    QString initial;
    std::function<std::optional<QString>(const QString&, bool)> convert;
};
using Registry = std::map<QString, Descriptor>;

// Files use enum names, while the existing configuration widgets use enum numbers.
template <typename T>
void Add(Registry& registry, const T& setting) {
    using Value = std::decay_t<decltype(setting.GetDefault())>;
    registry.emplace(QString::fromStdString(setting.GetLabel()), Descriptor{
        QString::fromStdString(setting.DefaultToString()),
        [minimum = QString::fromStdString(setting.MinVal()).toLongLong(),
         maximum = QString::fromStdString(setting.MaxVal()).toLongLong()](const QString& value, bool to_file) -> std::optional<QString> {
            if constexpr (std::is_enum_v<Value>) {
                for (const auto& [name, id] : Settings::EnumMetadata<Value>::Canonicalizations()) {
                    const auto symbol = QString::fromUtf8(name.data(), static_cast<int>(name.size()));
                    const auto number = QString::number(static_cast<u32>(id));
                    if (value == (to_file ? number : symbol)) return to_file ? symbol : number;
                }
            } else if constexpr (std::is_same_v<Value, bool>) {
                if (value == QStringLiteral("true") || value == QStringLiteral("false")) return value;
            } else {
                bool ok{};
                const auto number = value.toLongLong(&ok);
                if (ok && number >= minimum && number <= maximum) return QString::number(number);
            }
            return std::nullopt;
        }});
}

Registry MakeRegistry() {
    Registry result;
    const auto add = [&](const auto& setting) { Add(result, setting); };
    const auto& s = Settings::values;
    add(s.renderer_backend);
    add(s.use_docked_mode);
    add(s.resolution_setup);
    add(s.aspect_ratio);
    add(s.vsync_mode);
    add(s.fps_limit);
    add(s.scaling_filter);
    add(s.fsr_sharpening_slider);
    add(s.anti_aliasing);
    add(s.gpu_accuracy);
    add(s.vram_usage_mode);
    add(s.accelerate_astc);
    add(s.astc_recompression);
    add(s.max_anisotropy);
    add(s.nvdec_emulation);
    add(s.frame_pacing_mode);
    add(s.use_disk_shader_cache);
    add(s.use_vulkan_driver_pipeline_cache);
    add(s.precache_shaders);
    add(s.use_asynchronous_shaders);
    add(s.async_presentation);
    add(s.native_async_draws);
    add(s.native_pipeline_wait_ms);
    add(s.nvidia_dlss);
    add(s.ocarina_fsr);
    add(s.amd_fsr4);
    add(s.oot_anti_aliasing);
    add(s.post_shader_enabled);
    add(s.frame_gen);
    add(s.frame_gen_type);
    add(s.frame_gen_multiplier);
    add(s.frame_gen_target_rate);
    add(s.frame_gen_flow_scale_auto);
    add(s.frame_gen_flow_scale);
    add(s.frame_gen_queue_target);
    return result;
}

QString Tr(const char* text) { return QCoreApplication::translate("GraphicsPresets", text); }

QJsonObject Builtin(const Registry& registry, int kind) {
    QJsonObject values;
    for (const auto& [key, descriptor] : registry) {
        const auto value = descriptor.convert(descriptor.initial, true);
        if (value) values.insert(key, *value);
    }
    const auto set = [&](const auto& setting, auto value) {
        const auto key = QString::fromStdString(setting.GetLabel());
        QString text;
        if constexpr (std::is_same_v<decltype(value), bool>) text = value ? QStringLiteral("true") : QStringLiteral("false");
        else text = QString::number(static_cast<qlonglong>(value));
        values.insert(key, *registry.at(key).convert(text, true));
    };
    const auto& s = Settings::values;
    set(s.renderer_backend, Settings::RendererBackend::Vulkan);
    set(s.gpu_accuracy, Settings::GpuAccuracy::Low);
    set(s.vram_usage_mode, Settings::VramUsageMode::Conservative);
    set(s.nvdec_emulation, Settings::NvdecEmulation::Gpu);
    set(s.accelerate_astc, Settings::AstcDecodeMode::Gpu);
    set(s.use_disk_shader_cache, true);
    set(s.use_vulkan_driver_pipeline_cache, true);
    set(s.precache_shaders, Settings::PrecacheShaders::Always);
    set(s.use_asynchronous_shaders, true);
    set(s.async_presentation, true);
    set(s.post_shader_enabled, false);
    set(s.frame_gen, false);
    set(s.nvidia_dlss, false);
    set(s.ocarina_fsr, false);
    set(s.amd_fsr4, false);
    set(s.resolution_setup, kind == 0 ? Settings::ResolutionSetup::Res3_4X :
        kind == 2 ? Settings::ResolutionSetup::Res2X : Settings::ResolutionSetup::Res1X);
    set(s.scaling_filter, kind == 0 || kind == 3 ? Settings::ScalingFilter::Bilinear : Settings::ScalingFilter::ScaleForce);
    set(s.anti_aliasing, kind == 0 || kind == 3 ? Settings::AntiAliasing::None :
        kind == 2 ? Settings::AntiAliasing::Smaa : Settings::AntiAliasing::Fxaa);
    set(s.max_anisotropy, kind == 0 || kind == 3 ? Settings::AnisotropyMode::Default :
        kind == 2 ? Settings::AnisotropyMode::X16 : Settings::AnisotropyMode::X4);
    set(s.astc_recompression, kind == 2 ? Settings::AstcRecompression::Uncompressed : Settings::AstcRecompression::Bc3);
    set(s.native_async_draws, kind == 2 ? Settings::NativeAsyncDraws::Wait : Settings::NativeAsyncDraws::Bounded);
    set(s.native_pipeline_wait_ms, 8);
    for (const auto* setting : std::initializer_list<const Settings::BasicSetting*>{&s.frame_gen_type,
         &s.frame_gen_multiplier, &s.frame_gen_target_rate, &s.frame_gen_flow_scale_auto,
         &s.frame_gen_flow_scale, &s.frame_gen_queue_target}) {
        values.remove(QString::fromStdString(setting->GetLabel()));
    }
    if (kind == 3) { set(s.use_docked_mode, Settings::ConsoleMode::Handheld);
    set(s.fps_limit, 30); }
    else { values.remove(QString::fromStdString(s.use_docked_mode.GetLabel())); values.remove(QString::fromStdString(s.fps_limit.GetLabel())); }
    return values;
}

QJsonObject Document(QString name, QJsonObject settings, QString game = {}) {
    QJsonObject out{{QStringLiteral("format"), QStringLiteral("lichii.graphics-preset")},
                    {QStringLiteral("version"), 1}, {QStringLiteral("name"), name},
                    {QStringLiteral("settings"), settings}};
    if (!game.isEmpty()) out.insert(QStringLiteral("game"), game);
    return out;
}

class Panel final : public QWidget {
public:
    Panel(QWidget& tab, QWidget& owner, bool running, u64 game)
        : QWidget(&tab), dialog(owner), registry(MakeRegistry()), title(game) {
        path = QString::fromStdString(Common::FS::PathToUTF8String(
            Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "graphics-presets.json"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(Tr("Graphics preset:"), this));
        combo = new QComboBox(this); row->addWidget(combo, 1);
        auto* apply = new QPushButton(Tr("Apply Preset"), this); row->addWidget(apply);
        auto* save = new QPushButton(Tr("Save As…"), this); row->addWidget(save);
        auto* manage = new QPushButton(Tr("Manage"), this); row->addWidget(manage);
        auto* menu = new QMenu(manage); manage->setMenu(menu);
        layout->addLayout(row);
        description = new QLabel(this); description->setWordWrap(true); layout->addWidget(description);
        apply->setEnabled(!running);
        if (running) apply->setToolTip(Tr("Stop the game to apply a graphics preset."));
        save->setToolTip(Tr("Saves the edited graphics controls immediately, even if this dialog is canceled."));
        connect(combo, &QComboBox::currentIndexChanged, this, [this] { Describe(); });
        connect(apply, &QPushButton::clicked, this, [this] { Apply(); });
        connect(save, &QPushButton::clicked, this, [this] { Save(false); });
        update = menu->addAction(Tr("Update"), this, [this] { Save(true); });
        rename = menu->addAction(Tr("Rename…"), this, [this] { Rename(); });
        remove = menu->addAction(Tr("Delete…"), this, [this] { Delete(); });
        menu->addSeparator();
        menu->addAction(Tr("Import…"), this, [this] { Import(); });
        menu->addAction(Tr("Export…"), this, [this] { Export(); });
        Load(); Refresh();
        auto* target = qobject_cast<QVBoxLayout*>(tab.layout());
        if (target) target->insertWidget(0, this);
    }
private:
    QWidget& dialog;
    Registry registry;
    u64 title;
    QString path;
    QJsonArray customs;
    bool writable = true;
    QComboBox* combo{};
    QLabel* description{};
    QAction *update{}, *rename{}, *remove{};
    std::vector<QJsonObject> entries;
    int custom_start{};

    void Error(const QString& text) { QMessageBox::warning(this, Tr("Graphics Presets"), text); }
    bool Valid(const QJsonObject& object) const {
        const auto name = object.value(QStringLiteral("name")).toString().trimmed();
        if (object.value(QStringLiteral("format")).toString() != QStringLiteral("lichii.graphics-preset") ||
            object.value(QStringLiteral("version")) != QJsonValue(1) || name.isEmpty() || name.size() > 64 ||
            !object.value(QStringLiteral("settings")).isObject()) return false;
        const auto game = object.value(QStringLiteral("game"));
        if (!game.isUndefined() && game.toString() != QStringLiteral("0400486024DEC000")) return false;
        const auto settings = object.value(QStringLiteral("settings")).toObject();
        if (settings.isEmpty()) return false;
        for (auto it = settings.begin(); it != settings.end(); ++it) {
            const auto found = registry.find(it.key());
            if (found == registry.end() || !it.value().isString() ||
                !found->second.convert(it.value().toString(), false)) return false;
        }
        return true;
    }
    bool Write(const QString& destination, const QJsonDocument& data) {
        QSaveFile file(destination);
        if (!file.open(QIODevice::WriteOnly)) { Error(file.errorString()); return false; }
        const auto bytes = data.toJson();
        if (bytes.size() > MaxFileSize) { Error(Tr("Preset file exceeds 1 MiB.")); return false; }
        if (file.write(bytes) != bytes.size() || !file.commit()) { Error(file.errorString()); return false; }
        return true;
    }
    void Load() {
        QFile file(path);
        if (!file.exists()) return;
        QJsonParseError error;
        if (!file.open(QIODevice::ReadOnly) || file.size() > MaxFileSize) { writable = false; Error(Tr("Cannot read the preset library.")); return; }
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        const auto object = document.object();
        if (error.error != QJsonParseError::NoError || object.value(QStringLiteral("version")) != QJsonValue(1) ||
            !object.value(QStringLiteral("presets")).isArray()) { writable = false; Error(Tr("Invalid preset library. The existing file will be preserved.")); return; }
        const auto list = object.value(QStringLiteral("presets")).toArray();
        for (const auto& entry : list) if (!entry.isObject() || !Valid(entry.toObject())) { writable = false; Error(Tr("Invalid preset library. The existing file will be preserved.")); return; }
        customs = list;
    }
    bool Commit(const QJsonArray& next) {
        if (!writable) { Error(Tr("Repair or move the invalid preset library before saving.")); return false; }
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) { Error(Tr("Cannot create the preset directory.")); return false; }
        if (!Write(path, QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("presets"), next}}))) return false;
        customs = next; Refresh(); return true;
    }
    void Refresh() {
        combo->blockSignals(true); combo->clear(); entries.clear();
        const QStringList names{Tr("Performance"), Tr("Balanced"), Tr("Quality"), Tr("Ocarina — 4 GB laptop")};
        for (int i = 0; i < 4; ++i) {
            if (i == 3 && title != Ocarina) continue;
            entries.push_back(Document(names[i], Builtin(registry, i), i == 3 ? QStringLiteral("0400486024DEC000") : QString{}));
            combo->addItem(names[i]);
        }
        custom_start = static_cast<int>(entries.size());
        if (!customs.isEmpty()) combo->insertSeparator(combo->count());
        for (const auto& item : customs) {
            auto object = item.toObject();
            entries.push_back(object); combo->addItem(object.value(QStringLiteral("name")).toString());
        }
        combo->blockSignals(false); Describe();
    }
    int Index() const { const int i = combo->currentIndex(); return i >= custom_start ? i - 1 : i; }
    QJsonObject Selected() const { const int i = Index(); return i >= 0 && i < static_cast<int>(entries.size()) ? entries[i] : QJsonObject{}; }
    void Describe() {
        const bool custom = Index() >= custom_start;
        update->setEnabled(custom && writable); rename->setEnabled(custom && writable); remove->setEnabled(custom && writable);
        const bool oot = Selected().contains(QStringLiteral("game"));
        description->setText(oot ? Tr("For 4 GB VRAM laptops: handheld, 30 FPS limit, and shader precaching. Effects may appear late while compiling. Disable Max Resolution in Add-ons. A stable 30 FPS is not guaranteed.") :
            Tr("Apply stages these values until you confirm the settings dialog. Saved presets persist immediately. Performance and Balanced may briefly omit effects while shaders compile."));
    }
    Fields FindFields() const {
        Fields fields;
        for (auto* field : dialog.findChildren<ConfigurationShared::Widget*>()) {
            const auto key = QString::fromStdString(field->SettingKey());
            if (registry.contains(key)) fields.emplace(key, field);
        }
        return fields;
    }
    QJsonObject Capture() const {
        QJsonObject out;
        const auto fields = FindFields();
        for (const auto& [key, descriptor] : registry) {
            const auto field = fields.find(key);
            const auto value = field != fields.end() ? QString::fromStdString(field->second->DraftValue()) :
                QString::fromStdString(Settings::IsConfiguringGlobal() ? Settings::values.linkage.by_key.at(key.toStdString())->ToStringGlobal() : Settings::values.linkage.by_key.at(key.toStdString())->ToString());
            if (field == fields.end()) {
                if (Settings::values.linkage.by_key.at(key.toStdString())->TypeId() == "bool")
                    out.insert(key, QStringLiteral("false"));
                continue;
            }
            if (const auto encoded = descriptor.convert(value, true)) out.insert(key, *encoded);
        }
        return out;
    }
    void changeEvent(QEvent* event) override {
        if (event->type() == QEvent::LanguageChange) {
            const int selected = combo->currentIndex();
            Refresh(); combo->setCurrentIndex(selected);
        }
        QWidget::changeEvent(event);
    }
    void Apply() {
        const auto preset = Selected();
        if (!Valid(preset)) { Error(Tr("Invalid graphics preset.")); return; }
        if (preset.contains(QStringLiteral("game")) && title != Ocarina) { Error(Tr("This preset is restricted to Ocarina of Time.")); return; }
        const auto fields = FindFields();
        const auto values = preset.value(QStringLiteral("settings")).toObject();
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (!fields.contains(it.key()) && it.value().toString() != QStringLiteral("false")) {
                Error(Tr("This preset contains an unavailable control: %1").arg(it.key())); return;
            }
        }
        // Validate every adapter first, then roll back the draft if a dynamic control rejects a value.
        std::map<QString, std::pair<std::string, bool>> previous;
        for (const auto& [key, field] : fields) previous.emplace(key, std::pair{field->DraftValue(), field->restore_button && field->restore_button->isEnabled()});
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (!fields.contains(it.key())) continue;
            auto* field = fields.at(it.key());
            const auto decoded = registry.at(it.key()).convert(it.value().toString(), false);
            if (!decoded || !field->SetDraftValue(decoded->toStdString())) {
                for (const auto& [key, state] : previous) {
                    auto* restore = fields.at(key); restore->SetDraftValue(state.first);
                    if (restore->restore_button) { restore->restore_button->setEnabled(state.second); restore->restore_button->setVisible(state.second); }
                }
                Error(Tr("The selected renderer or display does not support this preset.")); return;
            }
        }
    }
    std::optional<QString> Name(QString initial = {}) {
        bool ok{};
        const auto name = QInputDialog::getText(this, Tr("Save Graphics Preset"), Tr("Name (saved immediately):"), QLineEdit::Normal, initial, &ok).trimmed();
        if (!ok) return std::nullopt;
        if (name.isEmpty() || name.size() > 64) { Error(Tr("Use a name of 1–64 characters.")); return std::nullopt; }
        for (int i = 0; i < custom_start; ++i) if (name.compare(entries[i].value(QStringLiteral("name")).toString(), Qt::CaseInsensitive) == 0) { Error(Tr("Built-in preset names are reserved.")); return std::nullopt; }
        return name;
    }
    void Store(QJsonObject preset, int replace = -1) {
        auto next = customs;
        const auto name = preset.value(QStringLiteral("name")).toString();
        for (qsizetype i = 0; i < next.size(); ++i) {
            if (i != replace && name.compare(next[i].toObject().value(QStringLiteral("name")).toString(), Qt::CaseInsensitive) == 0) {
                if (QMessageBox::question(this, Tr("Replace Preset"), Tr("Replace the saved preset %1 immediately? Choose No to enter another name.").arg(name)) != QMessageBox::Yes) {
                    if (const auto renamed = Name(name)) { preset.insert(QStringLiteral("name"), *renamed); Store(preset, replace); } return;
                }
                if (replace >= 0) { Error(Tr("Another preset already uses that name.")); return; }
                replace = static_cast<int>(i); break;
            }
        }
        preset.insert(QStringLiteral("id"), replace >= 0 ? next[replace].toObject().value(QStringLiteral("id")) : QJsonValue(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (replace >= 0) next[replace] = preset; else next.append(preset);
        if (Commit(next)) combo->setCurrentIndex(custom_start + 1 + (replace >= 0 ? replace : static_cast<int>(next.size()) - 1));
    }
    void Save(bool overwrite) {
        if (overwrite && QMessageBox::question(this, Tr("Update Preset"), Tr("Replace this saved preset with the edited controls immediately?")) != QMessageBox::Yes) return;
        auto name = overwrite ? std::optional<QString>{Selected().value(QStringLiteral("name")).toString()} : Name();
        if (name) Store(Document(*name, Capture(), Selected().value(QStringLiteral("game")).toString()), overwrite ? Index() - custom_start : -1);
    }
    void Rename() { if (auto name = Name(Selected().value(QStringLiteral("name")).toString())) { auto preset = Selected(); preset.insert(QStringLiteral("name"), *name); Store(preset, Index() - custom_start); } }
    void Delete() {
        if (QMessageBox::question(this, Tr("Delete Preset"), Tr("Delete this saved preset immediately?")) != QMessageBox::Yes) return;
        auto next = customs; next.removeAt(Index() - custom_start); Commit(next);
    }
    void Import() {
        const auto source = QFileDialog::getOpenFileName(this, Tr("Import Graphics Preset"), {}, Tr("JSON files (*.json)"));
        if (source.isEmpty()) return;
        QFile file(source); QJsonParseError error;
        if (!file.open(QIODevice::ReadOnly) || file.size() > MaxFileSize) { Error(Tr("Cannot read preset, or file exceeds 1 MiB.")); return; }
        const auto data = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !data.isObject() || !Valid(data.object())) { Error(Tr("Unsupported or invalid graphics preset.")); return; }
        auto object = data.object();
        if (auto name = Name(object.value(QStringLiteral("name")).toString())) { object.insert(QStringLiteral("name"), *name); Store(object); }
    }
    void Export() {
        const auto destination = QFileDialog::getSaveFileName(this, Tr("Export Graphics Preset"), {}, Tr("JSON files (*.json)"));
        if (destination.isEmpty()) return;
        auto object = Selected(); object.remove(QStringLiteral("id")); Write(destination, QJsonDocument(object));
    }
};
} // namespace

void InstallGraphicsPresets(QWidget& graphics_tab, QWidget& dialog, bool powered_on, u64 title_id) {
    new Panel(graphics_tab, dialog, powered_on, title_id);
}
