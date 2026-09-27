#pragma once

// Headless self-test of the owned-state architecture (STUDIO_SELFTEST=1). Lives outside main.cpp because it
// is a TEST, not application startup: it builds real widget instances and drives the source/author/commit
// cascade with no window, and main.cpp's only business with it is one early-out.

// Returns the failure count (0 = all pass).
int runOwnedStateSelfTest();
