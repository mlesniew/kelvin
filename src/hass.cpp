#include <map>
#include <set>

#include <BLEDevice.h>

#include <ArduinoJson.h>
#include <PicoUtils.h>
#include <PicoSyslog.h>

#include "hass.h"
#include "globals.h"
#include "readings.h"

extern PicoSyslog::SimpleLogger syslog;
extern std::map<BLEAddress, Readings> readings;
extern std::map<BLEAddress, String> names;

namespace {

void autodiscovery(BLEAddress address, String name) {
    if (!HomeAssistant::autodiscovery_topic.length())
        return;

    syslog.printf("Sending Home Assistant autodiscovery for device %s (%s).\n",
                  address.toString().c_str(), name.c_str());

    struct Entity {
        const char * name;
        const char * friendly_name;
        const char * unit;
        int precission;
        bool diagnostic;
        const char * device_class;
    };

    static const Entity entities[] = {
        {"temperature", "Temperature", "°C", 1, false, "temperature"},
        {"humidity", "Humidity", "%", 1, false, "humidity"},
        {"battery_level", "Battery level", "%", 0, true, "battery"},
        {"battery_voltage", "Battery voltage", "V", 2, true, "voltage"},
    };

    for (const auto & entity : entities) {
        String dev_addr_without_colons = address.toString().c_str();
        dev_addr_without_colons.replace(":", "");

        auto unique_id = "kelvin_" + dev_addr_without_colons + "_" + entity.name;

        const String topic = HomeAssistant::autodiscovery_topic + "/sensor/" + unique_id + "/config";

        JsonDocument json;
        json["unique_id"] = unique_id;
        json["object_id"] = "kelvin_" + name + "_" + entity.name;
        json["name"] = entity.friendly_name;
        json["device_class"] = entity.device_class;
        json["expire_after"] = 60 * 3;
        json["suggested_display_precision"] = entity.precission;
        json["state_topic"] = "kelvin/" + dev_addr_without_colons + "/" + entity.name;
        json["unit_of_measurement"] = entity.unit;
        if (entity.diagnostic) {
            json["entity_category"] = "diagnostic";
        }

        auto dev = json["device"];
        dev["name"] = name;
        dev["manufacturer"] = "Xiaomi";
        dev["model"] = "LYWSD03MMC";
        dev["identifiers"][0] = dev_addr_without_colons;
        dev["connections"][0][0] = "mac";
        dev["connections"][0][1] = address.toString().c_str();
        dev["via_device"] = "kelvin_" + get_board_id();

        auto publish = HomeAssistant::mqtt.begin_publish(topic, measureJson(json), 0, true);
        serializeJson(json, publish);
        publish.send();
    }
}

void autodiscovery(BLEAddress address) {
    const auto it = names.find(address);
    if (it != names.end())
        autodiscovery(address, it->second);
}

void autodiscovery() {
    for (const auto & kv : readings) {
        autodiscovery(kv.first);
    }
}

}

namespace HomeAssistant {

PicoMQTT::Client mqtt;
String autodiscovery_topic;

void init() {
    mqtt.client_id = "kelvin_" + get_board_id();

    mqtt.connected_callback = [] {
        syslog.printf("Home Assistant MQTT at %s:%i connected.\n", mqtt.host.c_str(), mqtt.port);

        // send autodiscovery messages
        autodiscovery();
    };
}


void tick() {
    static PicoUtils::Stopwatch last_update;
    static std::set<BLEAddress> discovered_devices;

    mqtt.loop();

    if (!mqtt.connected()) {
        return;
    }

    if (last_update.elapsed() < 15) {
        return;
    }

    for (const auto & kv : readings) {
        auto address = kv.first;
        const auto & reading = kv.second;

        if (reading.age.elapsed() > last_update.elapsed()) {
            continue;
        }

        const auto it = names.find(address);

        if ((it != names.end()) && (discovered_devices.count(address) == 0)) {
            autodiscovery(address, it->second);
            discovered_devices.insert(address);
        }

        String dev_addr = address.toString().c_str();
        dev_addr.replace(":", "");

        mqtt.publish("kelvin/" + dev_addr + "/temperature", String(reading.temperature));
        mqtt.publish("kelvin/" + dev_addr + "/humidity", String(reading.humidity));
        mqtt.publish("kelvin/" + dev_addr + "/battery_level", String(reading.battery_level));
        mqtt.publish("kelvin/" + dev_addr + "/battery_voltage", String(reading.battery_voltage));
    }

    last_update.reset();
}

bool connected() {
    return mqtt.connected();
}

}
