// SPDX-License-Identifier: GPL-3.0-or-later

#include <future>
#include <QCheckBox>
#include <QDialog>
#include <QProgressBar>
#include <QPushButton>
#include <QTest>
#include "yuzu/shader_backlog_prompt.h"

using namespace std::chrono_literals;
using namespace VideoCommon;

namespace {
struct Harness {
    QWidget window;
    ShaderJobScheduler scheduler{ShaderJobConfig{}};
    std::promise<void> release;
    bool alive{true};
    bool enabled{true};
    bool eligible{true};
    bool can_show_dialog{true};
    bool paused{};
    bool synthetic{true};
    bool released{};
    int pause_requests{};
    int resumes{};
    u64 failures{};
    u64 cancellations{};
    ShaderBuildProgress snapshot{.supported = true, .queued = 64, .oldest = 2s};
    ShaderJobHandle job;
    ShaderBacklogPrompt prompt;

    Harness()
        : prompt(
              &window,
              {
                  .alive = [this] { return alive; },
                  .enabled = [this] { return enabled; },
                  .eligible = [this] { return eligible; },
                  .can_show_dialog = [this] { return can_show_dialog; },
                  .paused = [this] { return paused; },
                  .progress = [this] { return synthetic ? snapshot : scheduler.RuntimeProgress(); },
                  .capture = [this] { return scheduler.CaptureRuntimeBuilds(); },
                  .pause = [this] { ++pause_requests; },
                  .resume =
                      [this] {
                          ++resumes;
                          paused = false;
                      },
                  .report_failures =
                      [this](u64 failed, u64 cancelled) {
                          failures += failed;
                          cancellations += cancelled;
                      },
              }) {
        const auto gate = release.get_future().share();
        job = scheduler.Submit({
            .driver = [gate] { gate.wait(); },
            .runtime_pipeline = true,
        });
    }

    ~Harness() {
        Release();
    }

    void Release() {
        if (!released) {
            release.set_value();
            released = true;
        }
    }

    void Trigger() {
        const auto begin = ShaderBacklogPolicy::Clock::now();
        prompt.Tick(begin);
        prompt.Tick(begin + 2s);
    }

    void ShowChoice() {
        Trigger();
        paused = true;
        prompt.Tick();
    }

    QDialog* Dialog() {
        return window.findChild<QDialog*>(QStringLiteral("shaderBacklogDialog"));
    }
};
} // namespace

class ShaderBacklogTests : public QObject {
    Q_OBJECT
private slots:
    void detectionRequiresSustainedBacklogAndRecovery() {
        ShaderBacklogPolicy policy;
        const auto start = ShaderBacklogPolicy::Clock::time_point{};
        ShaderBuildProgress high{.supported = true, .queued = 64, .oldest = 2s};
        ShaderBuildProgress low{.supported = true};
        QVERIFY(!policy.Observe(high, true, start));
        QVERIFY(!policy.Observe(high, true, start + 1s));
        // A short burst clears before it can interrupt the game.
        QVERIFY(!policy.Observe(low, true, start + 1500ms));
        QVERIFY(!policy.Observe(high, true, start + 2s));
        QVERIFY(policy.Observe(high, true, start + 4s));
        policy.Prompted(start + 4s);
        // A cooldown alone cannot re-arm a backlog that never recovered.
        QVERIFY(!policy.Observe(high, true, start + 70s));
        QVERIFY(!policy.Observe(low, true, start + 71s));
        QVERIFY(!policy.Observe(low, true, start + 73s));
        QVERIFY(!policy.Observe(high, true, start + 74s));
        QVERIFY(policy.Observe(high, true, start + 76s));
        policy.Prompted(start + 76s);
        QVERIFY(!policy.Observe(low, true, start + 77s));
        QVERIFY(!policy.Observe(low, true, start + 79s));
        QVERIFY(!policy.Observe(high, true, start + 80s));
        QVERIFY(!policy.Observe(high, true, start + 82s));
        QVERIFY(policy.Observe(high, true, start + 136s));
        policy.Suppress(true);
        QVERIFY(!policy.Observe(high, true, start + 200s));
    }

    void ineligibleSamplesAndYoungBuildsDoNotTrigger() {
        ShaderBacklogPolicy policy;
        const auto start = ShaderBacklogPolicy::Clock::time_point{};
        ShaderBuildProgress young{.supported = true, .queued = 64};
        QVERIFY(!policy.Observe(young, true, start));
        QVERIFY(!policy.Observe(young, true, start + 2s));
        young.oldest = 2s;
        QVERIFY(!policy.Observe(young, false, start + 3s));
        QVERIFY(!policy.Observe(young, true, start + 4s));
        QVERIFY(policy.Observe(young, true, start + 6s));
    }

    void requiresPauseAcknowledgement() {
        Harness h;
        h.Trigger();
        QCOMPARE(h.pause_requests, 1);
        QCOMPARE(h.prompt.CurrentState(), ShaderBacklogPrompt::State::PauseRequested);
        QVERIFY(!h.Dialog());
        h.prompt.Tick();
        QVERIFY(!h.Dialog());
        h.paused = true;
        h.prompt.Tick();
        QVERIFY(h.Dialog());
    }

    void workerFinishesWhileGuestRemainsPaused() {
        Harness h;
        h.ShowChoice();
        h.synthetic = false;
        auto* wait = h.Dialog()->findChild<QPushButton*>(QStringLiteral("waitForShaderBuilds"));
        QVERIFY(wait);
        wait->click();
        QCOMPARE(h.prompt.CurrentState(), ShaderBacklogPrompt::State::Waiting);
        QVERIFY(h.paused);
        // No guest frames or further submissions are needed to finish the captured job.
        h.Release();
        QVERIFY(h.job.WaitFor(2s));
        h.prompt.Tick();
        QCOMPARE(h.resumes, 1);
        QVERIFY(!h.prompt.OwnsPause());
        QCOMPARE(h.failures, 0ULL);
    }

    void doesNotStackTheChoiceOverAnotherModal() {
        Harness h;
        h.Trigger();
        h.paused = true;
        h.can_show_dialog = false;
        h.prompt.Tick();
        QVERIFY(!h.Dialog());
        QCOMPARE(h.resumes, 0);
        h.can_show_dialog = true;
        h.prompt.Tick();
        QVERIFY(h.Dialog());
    }

    void continueDoesNotCancelCompilation_data() {
        QTest::addColumn<int>("action");
        QTest::newRow("continue") << 0;
        QTest::newRow("escape") << 1;
        QTest::newRow("close") << 2;
        QTest::newRow("continue-while-waiting") << 3;
    }

    void continueDoesNotCancelCompilation() {
        QFETCH(int, action);
        Harness h;
        h.ShowChoice();
        auto* dialog = h.Dialog();
        QVERIFY(dialog);
        if (action == 3) {
            dialog->findChild<QPushButton*>(QStringLiteral("waitForShaderBuilds"))->click();
        }
        if (action == 0 || action == 3) {
            dialog->findChild<QPushButton*>(QStringLiteral("continuePlaying"))->click();
        } else if (action == 1) {
            QTest::keyClick(dialog, Qt::Key_Escape);
        } else {
            dialog->close();
        }
        QCOMPARE(h.resumes, 1);
        QVERIFY(!h.job.Ready());
        h.Release();
        QVERIFY(h.job.WaitFor(2s));
        QCOMPARE(h.job.Status(), ShaderJobStatus::Done);
    }

    void externalPauseAndShutdownDoNotResume() {
        for (const bool shutdown : {false, true}) {
            Harness h;
            h.ShowChoice();
            if (shutdown) {
                h.alive = false;
                h.prompt.Tick();
            } else {
                h.prompt.Cancel();
            }
            QCOMPARE(h.resumes, 0);
            QVERIFY(!h.prompt.OwnsPause());
            QVERIFY(h.paused);
        }
    }

    void staleChoiceAndDisabledOptionResumeOnlyOwnedPause() {
        for (const bool disable : {false, true}) {
            Harness h;
            h.Trigger();
            h.paused = true;
            if (disable) {
                h.enabled = false;
            } else {
                h.snapshot.queued = 0;
            }
            h.prompt.Tick();
            QCOMPARE(h.resumes, 1);
            QVERIFY(!h.Dialog());
        }
    }

    void disabledAndUnsupportedNeverPause() {
        Harness h;
        h.enabled = false;
        h.Trigger();
        QCOMPARE(h.pause_requests, 0);
        h.enabled = true;
        h.snapshot.supported = false;
        h.Trigger();
        QCOMPARE(h.pause_requests, 0);
        h.snapshot.supported = true;
        h.eligible = false;
        h.Trigger();
        QCOMPARE(h.pause_requests, 0);
    }

    void abandonedDialogCannotRestartAWait() {
        Harness h;
        h.ShowChoice();
        auto* old_dialog = h.Dialog();
        auto* wait = old_dialog->findChild<QPushButton*>(QStringLiteral("waitForShaderBuilds"));
        auto* play = old_dialog->findChild<QPushButton*>(QStringLiteral("continuePlaying"));
        h.prompt.Cancel();
        wait->click();
        play->click();
        QCOMPARE(h.prompt.CurrentState(), ShaderBacklogPrompt::State::Monitoring);
        QCOMPARE(h.resumes, 0);
    }

    void failuresAndCancellationFinishTheWait() {
        Harness h;
        auto cancelled = h.scheduler.Submit({.driver = [] {}, .runtime_pipeline = true});
        auto failed = h.scheduler.Submit({
            .driver = [] { throw std::runtime_error("test build failure"); },
            .runtime_pipeline = true,
        });
        h.ShowChoice();
        h.synthetic = false;
        h.Dialog()->findChild<QPushButton*>(QStringLiteral("waitForShaderBuilds"))->click();
        h.scheduler.Cancel(cancelled);
        h.Release();
        QVERIFY(h.job.WaitFor(2s));
        QVERIFY(failed.WaitFor(2s));
        h.prompt.Tick();
        QCOMPARE(h.failures, 1ULL);
        QCOMPARE(h.cancellations, 1ULL);
        QCOMPARE(h.resumes, 1);
    }

    void laterBuildsDoNotInflateTheCapturedTotal() {
        Harness h;
        h.ShowChoice();
        h.synthetic = false;
        h.Dialog()->findChild<QPushButton*>(QStringLiteral("waitForShaderBuilds"))->click();
        std::promise<void> release_later;
        const auto later_gate = release_later.get_future().share();
        auto later = h.scheduler.Submit({
            .driver = [later_gate] { later_gate.wait(); },
            .runtime_pipeline = true,
        });
        h.prompt.Tick();
        QCOMPARE(h.Dialog()->findChild<QProgressBar*>()->maximum(), 1);
        h.Release();
        QVERIFY(h.job.WaitFor(2s));
        h.prompt.Tick();
        QCOMPARE(h.resumes, 1);
        QVERIFY(!later.Ready());
        release_later.set_value();
        QVERIFY(later.WaitFor(2s));
    }
};

QTEST_MAIN(ShaderBacklogTests)
#include "qt_shader_backlog.moc"
