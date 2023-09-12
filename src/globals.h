#pragma once

#include <mutex>
#include <PicoPrometheus.h>

extern std::mutex mutex;

const String & get_board_id();

extern PicoPrometheus::Registry & get_prometheus();

#define prometheus get_prometheus()
