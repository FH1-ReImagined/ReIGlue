// Private runtime state. Never included by SDK clients.
#pragma once

#include <unordered_set>

namespace rex::ui {

// Accessed only on the UI thread, like ImGuiDrawer's dialog registrations.
class DialogRepaintHints {
 public:
  void SetContinuous(const void* dialog, bool continuous) {
    if (!dialog) return;
    if (continuous) passive_.erase(dialog);
    else passive_.insert(dialog);
  }
  void Forget(const void* dialog) { passive_.erase(dialog); }
  bool WantsContinuous(const void* dialog) const { return !passive_.contains(dialog); }

 private:
  std::unordered_set<const void*> passive_;
};

}  // namespace rex::ui
