#include "globals.h"

std::mutex mutex;

PicoPrometheus::Registry & get_prometheus() {
    static PicoPrometheus::SynchronizedRegistry<std::mutex> prometheus_registry(mutex);
    return prometheus_registry;
}
