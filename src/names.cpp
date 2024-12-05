#include <SPIFFS.h>

#include <PicoUtils.h>

#include "names.h"

namespace {
const char NAMES_PATH[] PROGMEM = "/names.json";
}

void Names::load() {
    names.clear();
    PicoUtils::JsonConfigFile<JsonDocument> json(SPIFFS, FPSTR(NAMES_PATH));
    for (auto kv : json.as<JsonObject>()) {
        BLEAddress address(kv.key().c_str());
        const String name = kv.value().as<const char *>();

        if (name.length() == 0) {
            continue;
        }
        names[address] = name;
    }
}

JsonDocument Names::json() const {
    JsonDocument json;

    for (auto & kv : names) {
        const auto & address = kv.first;
        const String address_str = BLEAddress(address).toString().c_str();
        const auto & name = kv.second;
        json[address_str] = name;
    }

    return json;
}

void Names::save() {
    auto file = SPIFFS.open(FPSTR(NAMES_PATH), "w");
    auto config = json();
    serializeJson(config, file);
    if (file) {
        file.close();
    }
    dirty = false;
}

const char * Names::operator[](const BLEAddress & address) const {
    const auto it = names.find(address);
    return it != names.end() ? it->second.c_str() : nullptr;
}

void Names::set(const BLEAddress & address, const String & name) {
    names[address] = name;
    dirty = true;
}
