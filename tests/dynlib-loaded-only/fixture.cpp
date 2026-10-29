// Copyright (c) 2026 Sean O'Connor. BSD 3-Clause; see LICENSE.
#if defined(_WIN32)
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" FIXTURE_EXPORT int DynlibFixtureValue() {
  return 511;
}
