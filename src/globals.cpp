#include "globals.h"

std::mutex mutex;

const String & get_board_id() {
    static const String board_id((uint32_t)(ESP.getEfuseMac() >> 24), HEX);
    return board_id;
}

PicoPrometheus::Registry & get_prometheus() {
    static PicoPrometheus::SynchronizedRegistry<std::mutex> prometheus_registry(mutex);
    return prometheus_registry;
}
