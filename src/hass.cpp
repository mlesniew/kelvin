#include <map>
#include <set>

#include <BLEDevice.h>

#include <PicoUtils.h>
#include <PicoSyslog.h>

#include "hass.h"
#include "globals.h"
#include "reading.h"

extern PicoSyslog::SimpleLogger syslog;
extern std::map<BLEAddress, BluetoothDevice> devices;

namespace {

void autodiscovery(const BluetoothDevice & device) {
    if (!HomeAssistant::autodiscovery_topic.length())
        return;

    syslog.printf("Sending Home Assistant autodiscovery for device %s (%s).\n",
                  device.address.c_str(), device.name.c_str());

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
        String dev_addr_without_colons = device.address;
        dev_addr_without_colons.replace(":", "");

        auto unique_id = "kelvin_" + dev_addr_without_colons + "_" + entity.name;

        const String topic = HomeAssistant::autodiscovery_topic + "/sensor/" + unique_id + "/config";

        JsonDocument json;
        json["unique_id"] = unique_id;
        json["object_id"] = "kelvin_" + device.name + "_" + entity.name;
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
        dev["name"] = device.name;
        dev["manufacturer"] = "Xiaomi";
        dev["model"] = "LYWSD03MMC";
        dev["identifiers"][0] = dev_addr_without_colons;
        dev["connections"][0][0] = "mac";
        dev["connections"][0][1] = device.address.c_str();
        dev["via_device"] = "kelvin_" + get_board_id();

        auto publish = HomeAssistant::mqtt.begin_publish(topic, measureJson(json), 0, true);
        serializeJson(json, publish);
        publish.send();
    }
}

void autodiscovery() {
    for (const auto & kv : devices) {
        const auto & device = kv.second;
        if (device.name.length()) {
            autodiscovery(device);
        }
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
    static std::set<String> discovered_devices;

    mqtt.loop();

    if (!mqtt.connected()) {
        return;
    }

    if (last_update.elapsed() < 15) {
        return;
    }

    for (const auto & kv : devices) {
        const auto & device = kv.second;
        const auto * readings = device.get_readings();
        if (!readings) {
            continue;
        }
        if (readings->last_update.elapsed() > last_update.elapsed()) {
            continue;
        }

        if (device.name.length() && !discovered_devices.count(device.address)) {
            autodiscovery(device);
            discovered_devices.insert(device.address);
        }

        String dev_addr = device.address;
        dev_addr.replace(":", "");

        mqtt.publish("kelvin/" + dev_addr + "/temperature", String(readings->temperature));
        mqtt.publish("kelvin/" + dev_addr + "/humidity", String(readings->humidity));
        mqtt.publish("kelvin/" + dev_addr + "/battery_level", String(readings->battery_level));
        mqtt.publish("kelvin/" + dev_addr + "/battery_voltage", String(readings->battery_voltage));
    }

    last_update.reset();
}

bool connected() {
    return mqtt.connected();
}

}
