#include <map>
#include <set>

#include <BLEDevice.h>

#include <ArduinoJson.h>
#include <PicoUtils.h>
#include <PicoSyslog.h>

#include "hass.h"
#include "globals.h"
#include "readings.h"

extern "C" uint8_t temprature_sens_read();

extern String hostname;
extern PicoSyslog::SimpleLogger syslog;
extern std::map<BLEAddress, Readings> readings;
extern std::map<BLEAddress, String> names;
extern PicoMQTT::Client mqtt;

namespace {

struct Entity {
    const char * name;
    const char * friendly_name;
    const char * unit;
    int precission;
    bool binary;
    bool diagnostic;
    const char * device_class;
};

void autodiscovery(BLEAddress address, String name) {
    if (!HomeAssistant::autodiscovery_topic.length()) {
        return;
    }

    syslog.printf("Sending Home Assistant autodiscovery for device %s (%s).\n",
                  address.toString().c_str(), name.c_str());

    static const Entity entities[] = {
        {"temperature", "Temperature", "°C", 1, false, false, "temperature"},
        {"humidity", "Humidity", "%", 1, false, false, "humidity"},
        {"battery_level", "Battery level", "%", 0, false, true, "battery"},
        {"battery_voltage", "Battery voltage", "V", 2, false, true, "voltage"},
    };

    const String mac = address.toString().c_str();
    String dev_addr_without_colons = mac;
    dev_addr_without_colons.replace(":", "");

    for (const auto & entity : entities) {
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

        auto dev = json["device"].to<JsonObject>();
        dev["name"] = name;
        dev["manufacturer"] = "Xiaomi";
        dev["model"] = "LYWSD03MMC";
        dev["identifiers"][0] = dev_addr_without_colons;
        dev["connections"][0][0] = "mac";
        dev["connections"][0][1] = mac;
        dev["via_device"] = "kelvin_" + get_board_id();

        auto publish = HomeAssistant::mqtt.begin_publish(topic, measureJson(json), 0, true);
        serializeJson(json, publish);
        publish.send();
    }

}

void autodiscovery(BLEAddress address) {
    const auto it = names.find(address);
    if (it != names.end()) {
        autodiscovery(address, it->second);
    }
}

void autodiscovery() {
    for (const auto & kv : readings) {
        autodiscovery(kv.first);
    }

    static const Entity entities[] = {
        {"rssi", "WiFi RSSI", "dBm", 0, false, true, "signal_strength"},
        {"uptime", "Uptime", "s", 0, false, true, "duration"},
        {"free_heap", "Free Heap", "kB", 0, false, true, "data_size"},
        {"temperature", "Temperature", "°C", 0, false, true, "temperature"},
        {"mqtt_connection", "MQTT", nullptr, 0, true, true, "connectivity"},
        {"connected_devices", "Connected devices", "devices", 0, false, true, nullptr},
        {"known_devices", "Known devices", "devices", 0, false, true, nullptr},
    };

    for (const auto & entity : entities) {
        JsonDocument json;

        auto unique_id = "kelvin_" + get_board_id() + "_" + entity.name;
        const String topic = HomeAssistant::autodiscovery_topic + (entity.binary ? "/binary_sensor/" : "/sensor/") + unique_id +
                             "/config";

        json["unique_id"] = unique_id;
        json["object_id"] = "kelvin_" + hostname + "_" + entity.name;
        json["name"] = entity.friendly_name;
        json["state_topic"] = "kelvin/" + get_board_id() + "/" + entity.name;
        json["availability_topic"] = HomeAssistant::mqtt.will.topic;

        if (!entity.binary) {
            json["suggested_display_precision"] = entity.precission;
        }

        if (entity.device_class) {
            json["device_class"] = entity.device_class;
        }

        if (entity.unit) {
            json["unit_of_measurement"] = entity.unit;
        }

        if (entity.diagnostic) {
            json["entity_category"] = "diagnostic";
        }

        auto dev = json["device"].to<JsonObject>();
        dev["name"] = hostname;
        dev["manufacturer"] = "mlesniew";
        dev["model"] = "Kelvin";
        dev["identifiers"][0] = "kelvin_" + get_board_id();
        dev["connections"][0][0] = "mac";
        dev["connections"][0][1] = WiFi.macAddress();
        dev["connections"][1][0] = "ip";
        dev["connections"][1][1] = WiFi.localIP();
        dev["sw_version"] = __DATE__ " " __TIME__;
        dev["configuration_url"] = String("http://") + WiFi.localIP().toString();

        auto publish = HomeAssistant::mqtt.begin_publish(topic, measureJson(json), 0, true);
        serializeJson(json, publish);
        publish.send();
    }
}

}

namespace HomeAssistant {

PicoMQTT::Client mqtt;
String autodiscovery_topic;

void publish_diagnostics() {
    mqtt.publish("kelvin/" + get_board_id() + "/rssi", String(WiFi.RSSI()));
    mqtt.publish("kelvin/" + get_board_id() + "/uptime", String(millis() / 1000));
    mqtt.publish("kelvin/" + get_board_id() + "/free_heap", String(double(ESP.getFreeHeap()) / 1024));
    mqtt.publish("kelvin/" + get_board_id() + "/temperature", String((double(temprature_sens_read()) - 32) / 1.8));
    mqtt.publish("kelvin/" + get_board_id() + "/mqtt_connection", ::mqtt.connected() ? "ON" : "OFF");

    const size_t devices = std::count_if(readings.begin(),
    readings.end(), [](const std::pair<const BLEAddress, Readings> & p) { return p.second.age.elapsed_millis() <= 3 * 60 * 1000; });
    mqtt.publish("kelvin/" + get_board_id() + "/connected_devices", String(devices));
    mqtt.publish("kelvin/" + get_board_id() + "/known_devices", String(names.size()));
}


void init() {
    mqtt.client_id = "kelvin_" + get_board_id();

    mqtt.will.topic = "kelvin/" + get_board_id() + "/availability";
    mqtt.will.payload = "offline";
    mqtt.will.retain = true;

    mqtt.connected_callback = [] {
        syslog.printf("Home Assistant MQTT at %s:%i connected.\n", mqtt.host.c_str(), mqtt.port);

        // send autodiscovery messages
        autodiscovery();

        // notify about availability
        mqtt.publish(mqtt.will.topic, "online", 0, true);

        // publish diagnostics right away
        publish_diagnostics();
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

    publish_diagnostics();

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
