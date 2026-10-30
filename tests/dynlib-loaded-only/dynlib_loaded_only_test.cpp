// Copyright (c) 2026 Sean O'Connor. BSD 3-Clause; see LICENSE.
#include <cassert>
#include <filesystem>
#include <iostream>
#include <utility>

#include <rex/platform/dynlib.h>

using rex::platform::DynamicLibrary;

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::filesystem::path fixture = argv[1];
  const auto missing = fixture.parent_path() / "missing-dynlib-fixture";
  assert(std::filesystem::is_regular_file(fixture));
  assert(!std::filesystem::exists(missing));

  // Installed on disk, but not injected: probing must not load it.
  DynamicLibrary probe;
  assert(!probe.LoadIfLoaded(fixture));
  assert(!probe);
  assert(probe.GetRawSymbol("DynlibFixtureValue") == nullptr);
  assert(!probe.LoadIfLoaded(missing));

  DynamicLibrary injector;
  assert(injector.Load(fixture));
  assert(probe.LoadIfLoaded(fixture));
  auto value = probe.GetSymbol<int (*)()>("DynlibFixtureValue");
  assert(value && value() == 511);
  assert(probe.GetRawSymbol("MissingSymbol") == nullptr);

  // The probe owns a reference, so releasing the original loader is safe.
  injector.Close();
  assert(value() == 511);
  {
    DynamicLibrary moved(std::move(probe));
    assert(!probe);
    assert(moved.GetSymbol<int (*)()>("DynlibFixtureValue")() == 511);
    DynamicLibrary assigned;
    assigned = std::move(moved);
    assert(!moved);
    assert(assigned.GetSymbol<int (*)()>("DynlibFixtureValue")() == 511);
  }
  // No leaked or pinned reference after the last owner dies.
  assert(!probe.LoadIfLoaded(fixture));

  assert(injector.Load(fixture));
  assert(probe.LoadIfLoaded(fixture));
  // A failed replacement clears the previous reference, as Load does.
  assert(!probe.LoadIfLoaded(missing));
  assert(!probe);
  injector.Close();
  assert(!probe.LoadIfLoaded(fixture));
  probe.Close();
  probe.Close();
  std::cout << "Loaded-only library probe: absent, installed, loaded, ownership and unload pass\n";
}
