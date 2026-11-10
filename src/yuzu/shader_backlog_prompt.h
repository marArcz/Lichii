// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include "yuzu/shader_backlog_policy.h"

class QDialog;
class QLabel;
class QProgressBar;
class QWidget;

class ShaderBacklogPrompt final : public QObject {
    Q_OBJECT

public:
    struct Access {
        std::function<bool()> alive;
        std::function<bool()> enabled;
        std::function<bool()> eligible;
        std::function<bool()> can_show_dialog;
        std::function<bool()> paused;
        std::function<VideoCommon::ShaderBuildProgress()> progress;
        std::function<VideoCommon::ShaderBuildGroup()> capture;
        std::function<void()> pause;
        std::function<void()> resume;
        std::function<void(u64, u64)> report_failures;
    };

    enum class State { Monitoring, PauseRequested, Choice, Waiting };

    ShaderBacklogPrompt(QWidget* parent, Access access);
    ~ShaderBacklogPrompt() override;
    // An external pause, resume or shutdown takes ownership: never resume it from this prompt.
    void Cancel();
    bool OwnsPause() const {
        return state != State::Monitoring;
    }
    State CurrentState() const {
        return state;
    }
    void Tick(ShaderBacklogPolicy::Clock::time_point now = ShaderBacklogPolicy::Clock::now());

private:
    void ShowChoice();
    void Finish();

    QWidget* const window;
    Access access;
    QTimer timer;
    ShaderBacklogPolicy policy;
    State state{State::Monitoring};
    QPointer<QDialog> dialog;
    QLabel* message{};
    QProgressBar* progress_bar{};
    VideoCommon::ShaderBuildGroup group;
    std::optional<ShaderBacklogPolicy::Clock::time_point> last_sample;
    u64 last_finished{};
};
