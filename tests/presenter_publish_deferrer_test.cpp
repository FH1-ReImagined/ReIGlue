// The presenter's publish deferrer (vulkan_present_on_submission_thread, FH1 #476; 0419: kept
// outside ui::Presenter so its layout and virtual table stay the nightly's). A fake presenter, no
// GPU or window: with a deferrer the step after the refresher (the backend's refresh-submitted
// callback, then making the image the next to present) runs only when the deferrer runs it, and
// the next refresh writes the next mailbox image only after that.
#include <rex/ui/presenter.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

#define CHECK(x)                                                                 \
  do {                                                                           \
    if (!(x)) {                                                                  \
      std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #x); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

namespace {

class FakePresenter final : public rex::ui::Presenter {
 public:
  explicit FakePresenter(bool with_callback) : Presenter(HostGpuLossCallback()) {
    if (with_callback) {
      SetGuestOutputRefreshSubmittedCallback(
          [this](uint32_t mailbox_index) { submitted.push_back(mailbox_index); });
    }
  }

  rex::ui::Surface::TypeFlags GetSupportedSurfaceTypes() const override { return 0; }
  bool CaptureGuestOutput(rex::ui::RawImage&) override { return false; }

  // A guest frame; returns the mailbox image the refresher wrote.
  uint32_t Frame() {
    RefreshGuestOutput(1280, 720, 16, 9, [](GuestOutputRefreshContext&) { return true; });
    return refreshed.back();
  }

  bool refresher_succeeds = true;
  std::vector<uint32_t> refreshed;
  std::vector<uint32_t> submitted;

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(rex::ui::Surface&,
                                                                            uint32_t, uint32_t,
                                                                            bool, bool&) override {
    return SurfacePaintConnectResult::kFailure;
  }
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override {}
  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t, uint32_t,
                              std::function<bool(GuestOutputRefreshContext&)>, bool&) override {
    refreshed.push_back(mailbox_index);
    return refresher_succeeds;
  }
  PaintResult PaintAndPresentImpl(bool) override { return PaintResult::kNotPresented; }
};

void RunSteps(std::vector<std::function<void()>>& steps) {
  for (auto& step : steps) {
    step();
  }
  steps.clear();
}

}  // namespace

int main() {
  std::vector<std::function<void()>> steps;
  auto deferrer = [&steps](std::function<void()> step) { steps.push_back(std::move(step)); };

  // Without a deferrer each refresh publishes its image at once: the next writes another one.
  {
    FakePresenter presenter(true);
    CHECK(!presenter.IsGuestOutputPublishDeferred());
    const uint32_t first = presenter.Frame();
    CHECK(presenter.Frame() != first);
    CHECK(presenter.submitted.empty());  // the backend closes the refresh itself then
  }

  // With a deferrer, the step runs when the deferrer runs it, the callback first.
  {
    FakePresenter presenter(true);
    presenter.SetGuestOutputPublishDeferrer(deferrer);
    CHECK(presenter.IsGuestOutputPublishDeferred());
    const uint32_t first = presenter.Frame();
    CHECK(steps.size() == 1);
    CHECK(presenter.submitted.empty());
    RunSteps(steps);
    CHECK(presenter.submitted.size() == 1 && presenter.submitted[0] == first);
    const uint32_t second = presenter.Frame();
    CHECK(second != first);  // published by the step
    RunSteps(steps);

    // A failed refresher: the image is not published, but its tracking is still closed by a step.
    presenter.refresher_succeeds = false;
    const uint32_t failed = presenter.Frame();
    CHECK(steps.size() == 1);
    RunSteps(steps);
    CHECK(presenter.submitted.back() == failed);
    presenter.refresher_succeeds = true;
    CHECK(presenter.Frame() == failed);
    RunSteps(steps);

    // Cleared: inline again.
    presenter.SetGuestOutputPublishDeferrer(nullptr);
    CHECK(!presenter.IsGuestOutputPublishDeferred());
    const size_t submitted = presenter.submitted.size();
    const uint32_t inline_first = presenter.Frame();
    CHECK(steps.empty());
    CHECK(presenter.Frame() != inline_first);
    CHECK(presenter.submitted.size() == submitted);
  }

  // A presenter without a backend callback still publishes through the deferrer, and a failed
  // refresher queues nothing.
  {
    FakePresenter presenter(false);
    presenter.SetGuestOutputPublishDeferrer(deferrer);
    const uint32_t first = presenter.Frame();
    CHECK(steps.size() == 1);
    RunSteps(steps);
    CHECK(presenter.Frame() != first);
    RunSteps(steps);
    presenter.refresher_succeeds = false;
    presenter.Frame();
    CHECK(steps.empty());
  }

  // A destroyed presenter's deferrer does not carry over to a new one (at the same address or not).
  for (int i = 0; i < 4; ++i) {
    auto presenter = std::make_unique<FakePresenter>(true);
    CHECK(!presenter->IsGuestOutputPublishDeferred());
    presenter->SetGuestOutputPublishDeferrer(deferrer);
  }

  std::printf("presenter publish deferrer: ok\n");
  return 0;
}
