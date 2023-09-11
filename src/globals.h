#pragma once

#include <mutex>
#include <PicoPrometheus.h>

extern std::mutex mutex;

extern PicoPrometheus::Registry & get_prometheus();

#define prometheus get_prometheus()
