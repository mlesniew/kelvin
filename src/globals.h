#pragma once

#include <Arduino.h>

#include <mutex>

extern std::mutex mutex;

const String & get_board_id();
