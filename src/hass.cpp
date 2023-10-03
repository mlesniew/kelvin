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
extern String hostname;

namespace {

void autodiscovery(const BluetoothDevice & device) {
    syslog.printf("Sending Home Assistant autodiscovery for device %s (%s).\n",
                  device.address.c_str(), device.name.c_str());

    const String board_unique_id = "kelvin-" + get_board_id();

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
        auto unique_id = board_unique_id + "-" + device.address + "-" + entity.name;
        unique_id.replace(":", "");

        const String topic = "homeassistant/sensor/" + unique_id + "/config";

        StaticJsonDocument<1024> json;
        json["unique_id"] = unique_id;
        json["name"] = entity.friendly_name;
        json["device_class"] = entity.device_class;
        json["expire_after"] = 60 * 3;
        json["suggested_display_precision"] = entity.precission;
        json["state_topic"] = "celsius/" + get_board_id() + "/" + device.address + "/" + entity.name;
        json["unit_of_measurement"] = entity.unit;
        if (entity.diagnostic) {
            json["entity_category"] = "diagnostic";
        }

        auto dev = json["device"];
        dev["name"] = "Sensor " + device.name + " (via " + hostname + ")";
        dev["identifiers"][0] = board_unique_id + "-" + device.address;
        dev["identifiers"][0] = board_unique_id + "-" + device.address;
        dev["via_device"] = board_unique_id;

        auto publish = HomeAssistant::mqtt.begin_publish(topic, measureJson(json), 0, true);
        serializeJson(json, publish);
        publish.send();
    }
}

void autodiscovery() {
    for (const auto & kv : devices) {
        const auto & device = kv.second;
        if (device.get_readings()) {
            autodiscovery(device);
        }
    }
}

}

namespace HomeAssistant {

PicoMQTT::Client mqtt;

void init() {
    mqtt.client_id = "kelvin-" + get_board_id();

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
        if (!discovered_devices.count(device.address)) {
            autodiscovery(device);
            discovered_devices.insert(device.address);
        }
        mqtt.publish("celsius/" + get_board_id() + "/" + device.address + "/temperature", String(readings->temperature));
        mqtt.publish("celsius/" + get_board_id() + "/" + device.address + "/humidity", String(readings->humidity));
        mqtt.publish("celsius/" + get_board_id() + "/" + device.address + "/battery_level", String(readings->battery_level));
        mqtt.publish("celsius/" + get_board_id() + "/" + device.address + "/battery_voltage",
                     String(readings->battery_voltage));
    }

    last_update.reset();
}

}
