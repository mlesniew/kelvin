#include <mutex>
#include <map>

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <uri/UriRegex.h>

#include <ArduinoJson.h>
#include <PicoMQ.h>
#include <PicoMQTT.h>
#include <PicoSyslog.h>
#include <PicoUtils.h>
#include <WiFiManager.h>

#include "globals.h"
#include "reading.h"
#include "hass.h"

PicoUtils::PinInput button(0, true);
PicoUtils::PinOutput wifi_led(2, false);

String hostname;
String ota_password;

PicoUtils::RestfulServer<WebServer> server;
PicoMQ picomq;
PicoMQTT::Client mqtt;
PicoSyslog::Logger syslog("kelvin");

std::map<BLEAddress, BluetoothDevice> devices;

bool active_scan_enabled;
PicoUtils::Stopwatch active_scan_stopwatch;
PicoUtils::Stopwatch last_mqtt_reconnect;
PicoUtils::WiFiControlSmartConfig wifi_control(wifi_led);

class ScanCallbacks: public BLEAdvertisedDeviceCallbacks {
    public:
        static bool active_scan_required;

        void onResult(BLEAdvertisedDevice advertisedDevice) override {
            std::lock_guard<std::mutex> guard(mutex);
            static const char ADDRESS_PREFIX[] = {0xa4, 0xc1, 0x38};

            auto address = advertisedDevice.getAddress();
            if (memcmp(address.getNative(), ADDRESS_PREFIX, 3) != 0) {
                return;
            }

            auto emplace_result = devices.emplace(address, address);
            auto & device = emplace_result.first->second;
            const bool is_new_element = emplace_result.second;

            device.update(advertisedDevice);

            active_scan_required = active_scan_required || (is_new_element && !device.name.length());
        }
} scan_callbacks;

bool ScanCallbacks::active_scan_required;

namespace network_config {

const char CONFIG_PATH[] PROGMEM = "/network.json";

void load() {
    PicoUtils::JsonConfigFile<JsonDocument> config(SPIFFS, FPSTR(CONFIG_PATH));
    const String default_hostname = "kelvin_" + get_board_id();
    hostname = config["hostname"] | default_hostname;
    mqtt.host = config["mqtt"]["server"] | "calor.local";
    mqtt.port = config["mqtt"]["port"] | 1883;
    mqtt.username = config["mqtt"]["username"] | "kelvin";
    mqtt.password = config["mqtt"]["password"] | "harara";
    syslog.server = config["syslog"] | "192.168.1.100";
    syslog.host = hostname;
    ota_password = config["ota_password"] | "";
    HomeAssistant::mqtt.host = config["hass"]["server"] | "";
    HomeAssistant::mqtt.port = config["hass"]["port"] | 1883;
    HomeAssistant::mqtt.username = config["hass"]["username"] | "";
    HomeAssistant::mqtt.password = config["hass"]["password"] | "";
    HomeAssistant::autodiscovery_topic = config["hass"]["autodiscovery_topic"] | "homeassistant";
}

void save() {
    auto file = SPIFFS.open(FPSTR(CONFIG_PATH), "w");
    if (file) {
        JsonDocument config;
        config["hostname"] = hostname;
        config["mqtt"]["host"] = mqtt.host;
        config["mqtt"]["port"] = mqtt.port;
        config["mqtt"]["username"] = mqtt.username;
        config["mqtt"]["password"] = mqtt.password;
        config["syslog"] = syslog.server;
        config["ota_password"] = ota_password;
        config["hass"]["server"] = HomeAssistant::mqtt.host;
        config["hass"]["port"] = HomeAssistant::mqtt.port;
        config["hass"]["username"] = HomeAssistant::mqtt.username;
        config["hass"]["password"] = HomeAssistant::mqtt.password;
        serializeJson(config, file);
        file.close();
    }
}

}

void restart_scan() {
    auto & scan = *BLEDevice::getScan();
    scan.stop();

    scan.setActiveScan(active_scan_enabled);
    scan.setInterval(100);
    scan.setWindow(99);

    scan.setAdvertisedDeviceCallbacks(
        &scan_callbacks,
        true /* allow duplicates */,
        true /* parse */);

    // scan forever
    scan.start(0, nullptr, false);
}

void setup() {
    wifi_led.init();
    button.init();

    Serial.begin(115200);

    SPIFFS.begin();
    network_config::load();

    Serial.println(F("\n\n"
                     "88  dP 888888 88     Yb    dP 88 88b 88\n"
                     "88odP  88__   88      Yb  dP  88 88Yb88\n"
                     "88\"Yb  88\"\"   88  .o   YbdP   88 88 Y88\n"
                     "88  Yb 888888 88ood8    YP    88 88  Y8\n"
                     "\n"
                     "Kelvin " __DATE__ " " __TIME__ "\n"
                     "\n\n"
                     "Press and hold button now to enter WiFi setup.\n"
                    ));

    WiFi.hostname(hostname);
    wifi_control.init(button);

    {
        BLEDevice::init("");
        active_scan_enabled = false;
        restart_scan();
    }

    server.on("/readings", HTTP_GET, [] {
        std::lock_guard<std::mutex> guard(mutex);

        JsonDocument json;

        for (const auto & kv : devices) {
            auto address = kv.first;
            const auto & device = kv.second;
            if (!device.get_readings()) {
                continue;
            }
            json[address.toString()] = device.get_json();
        }

        server.sendJson(json);
    });

    server.on("/devices", HTTP_GET, [] {
        std::lock_guard<std::mutex> guard(mutex);

        JsonDocument json;

        for (const auto & kv : devices) {
            auto address = kv.first;
            const auto & device = kv.second;
            json[address.toString()] = device.name.length() ? device.name.c_str() : (char *) 0;
        }

        server.sendJson(json);
    });

    mqtt.connected_callback = [] {
        syslog.println("MQTT connected, publishing readings...");
        last_mqtt_reconnect.reset();
    };

    server.begin();
    picomq.begin();
    mqtt.begin();

    HomeAssistant::init();

    {
        ArduinoOTA.setHostname(hostname.c_str());
        if (ota_password.length()) {
            ArduinoOTA.setPassword(ota_password.c_str());
        }
        ArduinoOTA.begin();
    }

    wifi_control.get_connectivity_level = []{
        unsigned int ret = 1;
        if (mqtt.connected()) ++ret;
        if (HomeAssistant::connected()) ++ret;
        return ret;
    };
}

void publish_readings() {
    static PicoUtils::Stopwatch last_publish;

    bool got_all_names = true;

    const bool just_reconnected = last_publish.elapsed() >= last_mqtt_reconnect.elapsed();

    for (const auto & kv : devices) {
        const auto & device = kv.second;

        if (!device.name.length()) {
            got_all_names = false;
        }

        const auto * readings = device.get_readings();
        if (!readings) {
            continue;
        }

        const bool already_published = (readings->last_update.elapsed() > last_publish.elapsed());
        const bool recent = (readings->last_update.elapsed() <= 120);

        if (already_published && !(recent && just_reconnected)) {
            // already published and we haven't just reconnected
            continue;
        }

        static const String topic_prefix = "celsius/" + get_board_id() + "/";
        const String topic = topic_prefix + device.address;

        if (device.name.length()) {
            picomq.publish(topic_prefix + device.name + "/temperature", readings->temperature);
            picomq.publish(topic_prefix + device.name + "/humidity", readings->humidity);
            mqtt.publish(topic_prefix + device.name + "/temperature", String(readings->temperature));
        }

        picomq.publish(topic_prefix + device.address + "/temperature", readings->temperature);
        picomq.publish(topic_prefix + device.address + "/humidity", readings->humidity);
    }

    last_publish.reset();

    if (active_scan_enabled && (got_all_names || (active_scan_stopwatch.elapsed() >= 3 * 60))) {
        syslog.println(F("Disabling active scan."));
        active_scan_enabled = false;
        restart_scan();
    } else if (ScanCallbacks::active_scan_required) {
        active_scan_stopwatch.reset();
        if (!active_scan_enabled) {
            syslog.println(F("Enabling active scan."));
            active_scan_enabled = true;
            restart_scan();
        }
    }

    ScanCallbacks::active_scan_required = false;
}

void no_wifi_reset() {
    static PicoUtils::Stopwatch stopwatch;

    if (WiFi.status() == WL_CONNECTED && (mqtt.host.isEmpty() || mqtt.connected())) {
        stopwatch.reset();
    } else if (stopwatch.elapsed() >= 5 * 60) {
        syslog.printf("No WiFi or MQTT connection for too long.  Resetting...");
        ESP.restart();
    }
}

void loop() {
    ArduinoOTA.handle();

    server.handleClient();
    picomq.loop();
    mqtt.loop();
    wifi_control.tick();

    {
        std::lock_guard<std::mutex> guard(mutex);
        publish_readings();
        HomeAssistant::tick();
    }

    no_wifi_reset();
}
