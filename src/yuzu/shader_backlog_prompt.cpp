// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <limits>
#include <utility>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include "common/logging.h"
#include "yuzu/shader_backlog_prompt.h"

ShaderBacklogPrompt::ShaderBacklogPrompt(QWidget* parent, Access access_)
    : QObject{parent}, window{parent}, access{std::move(access_)} {
    connect(&timer, &QTimer::timeout, this, [this] { Tick(); });
    timer.start(500);
}

ShaderBacklogPrompt::~ShaderBacklogPrompt() {
    Cancel();
}

void ShaderBacklogPrompt::Cancel() {
    state = State::Monitoring;
    group = {};
    if (dialog) {
        dialog->disconnect(this);
        for (auto* child : dialog->findChildren<QObject*>()) {
            child->disconnect(this);
        }
        dialog->hide();
        dialog->deleteLater();
        dialog = nullptr;
    }
    message = nullptr;
    progress_bar = nullptr;
}

void ShaderBacklogPrompt::Finish() {
    const bool resume = OwnsPause() && access.alive();
    Cancel();
    if (resume) {
        access.resume();
    }
}

void ShaderBacklogPrompt::Tick(ShaderBacklogPolicy::Clock::time_point now) {
    if (!access.alive()) {
        Cancel();
        return;
    }
    if (!access.enabled()) {
        if (OwnsPause()) {
            if (state == State::PauseRequested && !access.paused()) {
                return;
            }
            Finish();
        }
        policy.Observe({}, false, now);
        return;
    }
    const auto progress = access.progress();
    if (!progress.supported) {
        if (OwnsPause()) {
            Finish();
        }
        policy.Observe({}, false, now);
        return;
    }
    double throughput = 0;
    if (last_sample && now > *last_sample && progress.Finished() >= last_finished) {
        throughput = static_cast<double>(progress.Finished() - last_finished) /
                     std::chrono::duration<double>(now - *last_sample).count();
    }
    last_sample = now;
    last_finished = progress.Finished();
    switch (state) {
    case State::Monitoring:
        if (policy.Observe(progress, access.eligible(), now)) {
            // Another poll may have finished the backlog while eligibility was checked.
            if (access.progress().Pending() < ShaderBacklogPolicy::MinimumPendingBuilds) {
                return;
            }
            policy.Prompted(now);
            LOG_INFO(Frontend,
                     "Shader backlog: {} queued, {} running, oldest {} ms, {:.1f} builds/s",
                     progress.queued, progress.running,
                     std::chrono::duration_cast<std::chrono::milliseconds>(progress.oldest).count(),
                     throughput);
            state = State::PauseRequested;
            access.pause();
        }
        break;
    case State::PauseRequested:
        if (access.paused()) {
            if (progress.Pending() == 0) {
                Finish();
            } else if (access.can_show_dialog()) {
                ShowChoice();
            }
        }
        break;
    case State::Choice:
        if (progress.Pending() == 0) {
            Finish();
        } else {
            message->setText(
                tr("%n shader build(s) pending. Continuing may cause stutters or "
                   "temporarily missing effects, depending on your rendering settings.",
                   nullptr,
                   static_cast<int>(
                       std::min<u64>(progress.Pending(), std::numeric_limits<int>::max()))));
        }
        break;
    case State::Waiting: {
        const auto captured = group.Progress();
        const u64 total = captured.Finished() + captured.Pending();
        progress_bar->setMaximum(
            static_cast<int>(std::min<u64>(total, std::numeric_limits<int>::max())));
        progress_bar->setValue(
            static_cast<int>(std::min<u64>(captured.Finished(), std::numeric_limits<int>::max())));
        const u64 additional =
            progress.Pending() > captured.Pending() ? progress.Pending() - captured.Pending() : 0;
        message->setText(tr("Completed %1 of %2 builds. %3 additional build(s) pending.")
                             .arg(captured.Finished())
                             .arg(total)
                             .arg(additional));
        if (captured.Pending() == 0) {
            if (captured.failed || captured.cancelled) {
                access.report_failures(captured.failed, captured.cancelled);
            }
            Finish();
        }
        break;
    }
    }
}

void ShaderBacklogPrompt::ShowChoice() {
    state = State::Choice;
    dialog = new QDialog(window);
    dialog->setObjectName(QStringLiteral("shaderBacklogDialog"));
    dialog->setWindowTitle(tr("Shaders are still compiling"));
    dialog->setWindowModality(Qt::WindowModal);
    auto* layout = new QVBoxLayout(dialog);
    message = new QLabel(dialog);
    message->setWordWrap(true);
    message->setMinimumWidth(360);
    layout->addWidget(message);
    progress_bar = new QProgressBar(dialog);
    progress_bar->hide();
    layout->addWidget(progress_bar);
    auto* suppress = new QCheckBox(tr("Don't ask again this session"), dialog);
    layout->addWidget(suppress);
    connect(suppress, &QCheckBox::toggled, this,
            [this](bool checked) { policy.Suppress(checked); });
    auto* buttons = new QDialogButtonBox(dialog);
    auto* wait = buttons->addButton(tr("Wait for builds"), QDialogButtonBox::ActionRole);
    wait->setObjectName(QStringLiteral("waitForShaderBuilds"));
    auto* play = buttons->addButton(tr("Continue playing"), QDialogButtonBox::RejectRole);
    play->setObjectName(QStringLiteral("continuePlaying"));
    play->setDefault(true);
    layout->addWidget(buttons);
    connect(play, &QPushButton::clicked, this, &ShaderBacklogPrompt::Finish);
    connect(dialog, &QDialog::rejected, this, &ShaderBacklogPrompt::Finish);
    connect(wait, &QPushButton::clicked, this, [this, wait, suppress] {
        if (state != State::Choice) {
            return;
        }
        group = access.capture();
        state = State::Waiting;
        wait->hide();
        suppress->hide();
        progress_bar->show();
        Tick();
    });
    dialog->show();
    Tick();
}
