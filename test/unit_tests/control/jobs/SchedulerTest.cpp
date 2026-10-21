#include <chrono>
#include <future>

#include <gtest/gtest.h>

#include "control/jobs/Job.h"
#include "control/jobs/Scheduler.h"

namespace {
using namespace std::chrono_literals;

class SignallingJob: public Job {
public:
    SignallingJob(JobType type, std::promise<void>& completed): type(type), completed(completed) {}
    JobType getType() override { return type; }

protected:
    void run() override { completed.set_value(); }

private:
    JobType type;
    std::promise<void>& completed;
};

void enqueue(Scheduler& scheduler, JobType type, std::promise<void>& completed) {
    auto* job = new SignallingJob(type, completed);
    scheduler.addJob(job, JOB_PRIORITY_URGENT);
    job->unref();
}
}  // namespace

TEST(Scheduler, PinchHoldsRenderingAcrossPausesWhileOtherJobsContinue) {
    std::promise<void> rendered;
    std::promise<void> previewed;
    auto renderResult = rendered.get_future();
    auto previewResult = previewed.get_future();
    Scheduler scheduler;

    scheduler.beginZoomGesture();
    scheduler.blockRerenderZoom();
    enqueue(scheduler, JOB_TYPE_RENDER, rendered);
    scheduler.start();

    // Outlast the normal zoom debounce, then wake the worker with other work.
    EXPECT_EQ(renderResult.wait_for(350ms), std::future_status::timeout);
    enqueue(scheduler, JOB_TYPE_PREVIEW, previewed);
    EXPECT_EQ(previewResult.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(renderResult.wait_for(50ms), std::future_status::timeout);

    scheduler.endZoomGesture();
    EXPECT_EQ(renderResult.wait_for(2s), std::future_status::ready);
    scheduler.stop();
}

TEST(Scheduler, EndingPinchReleasesRenderingWithoutMainLoopOrDebounce) {
    std::promise<void> rendered;
    auto result = rendered.get_future();
    Scheduler scheduler;

    scheduler.beginZoomGesture();
    enqueue(scheduler, JOB_TYPE_RENDER, rendered);
    scheduler.start();
    EXPECT_EQ(result.wait_for(50ms), std::future_status::timeout);

    scheduler.blockRerenderZoom();
    scheduler.endZoomGesture();
    // No GLib main loop is pumped: a lingering debounce cannot release this job.
    EXPECT_EQ(result.wait_for(2s), std::future_status::ready);
    scheduler.stop();
}
