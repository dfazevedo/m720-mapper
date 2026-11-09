#pragma once

#include <windows.h>

#include <chrono>
#include <functional>
#include <string_view>

namespace m720 {

using Log = std::function<void(std::string_view)>;

// Cancellation is a manual-reset event. The active event is signaled only while fully armed.
void run_mapper(HANDLE stop, HANDLE active, HANDLE waiting, const Log& log);
void diagnose(const Log& log);
void probe(const Log& log);
void listen(std::chrono::seconds duration, const Log& log);

} // namespace m720
