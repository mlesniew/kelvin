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
#include <PicoPrometheus.h>
#include <PicoSyslog.h>
#include <PicoUtils.h>
#include <WiFiManager.h>

#include "globals.h"
#include "reading.h"
#include "hass.h"

PicoUtils::PinInput button(0, true);
PicoUtils::PinOutput wifi_led(2, false);
PicoUtils::Blink led_blinker(wifi_led, 0, 91);

String hostname;
String ota_password;

PicoUtils::RestfulServer<WebServer> server;
PicoMQ picomq;
PicoSyslog::Logger syslog("kelvin");

std::map<BLEAddress, BluetoothDevice> devices;

bool active_scan_enabled;
PicoUtils::Stopwatch active_scan_stopwatch;

namespace Metrics {
PicoPrometheus::Gauge free_heap(prometheus, "esp_free_heap", "Free heap memory in bytes", [] { return ESP.getFreeHeap(); });
}

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
    PicoUtils::JsonConfigFile<StaticJsonDocument<512>> config(SPIFFS, FPSTR(CONFIG_PATH));
    const String default_hostname = "kelvin_" + get_board_id();
    hostname = config["hostname"] | default_hostname;
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
        StaticJsonDocument<512> config;
        config["hostname"] = hostname;
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

void config_mode() {
    led_blinker.set_pattern(0b100100100 << 9);

    WiFiManagerParameter param_hostname("hostname", "Hostname", hostname.c_str(), 64);
    WiFiManagerParameter param_syslog("syslog", "Syslog server", syslog.server.c_str(), 64);
    WiFiManagerParameter param_ota_password("ota_password", "OTA password", ota_password.c_str(), 64);

    WiFiManager wifi_manager;

    wifi_manager.addParameter(&param_hostname);
    wifi_manager.addParameter(&param_syslog);
    wifi_manager.addParameter(&param_ota_password);

    wifi_manager.startConfigPortal("Kelvin");

    hostname = param_hostname.getValue();

    hostname = param_hostname.getValue();
    syslog.server = param_syslog.getValue();
    ota_password = param_ota_password.getValue();

    network_config::save();
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
    led_blinker.set_pattern(0b10);
    PicoUtils::BackgroundBlinker bb(led_blinker);

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

    delay(3000);

    if (button) {
        config_mode();
    }

    WiFi.hostname(hostname);
    WiFi.setAutoReconnect(true);
    WiFi.softAPdisconnect(true);
    WiFi.begin();

    {
        BLEDevice::init("");
        active_scan_enabled = false;
        restart_scan();
    }

    server.on("/readings", HTTP_GET, [] {
        std::lock_guard<std::mutex> guard(mutex);

        DynamicJsonDocument json(2048);

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

        DynamicJsonDocument json(2048);

        for (const auto & kv : devices) {
            auto address = kv.first;
            const auto & device = kv.second;
            json[address.toString()] = device.name.length() ? device.name.c_str() : (char *) 0;
        }

        server.sendJson(json);
    });

    prometheus.labels["module"] = "kelvin";
    prometheus.labels["board"] = get_board_id();

    prometheus.register_metrics_endpoint(server);
    server.begin();
    picomq.begin();

    HomeAssistant::init();

    {
        ArduinoOTA.setHostname(hostname.c_str());
        if (ota_password.length()) {
            ArduinoOTA.setPassword(ota_password.c_str());
        }
        ArduinoOTA.begin();
    }

    led_blinker.set_pattern(1);
}

void publish_readings() {

    static PicoUtils::Stopwatch last_publish;

    bool got_all_names = true;

    for (const auto & kv : devices) {
        const auto & device = kv.second;

        if (!device.name.length()) {
            got_all_names = false;
        }

        const auto * readings = device.get_readings();
        if (!readings) {
            continue;
        }

        if (readings->last_update.elapsed() > last_publish.elapsed()) {
            // already published
            continue;
        }

        static const String topic_prefix = "celsius/" + get_board_id() + "/";
        const String topic = topic_prefix + device.address;
        const auto json = device.get_json();

        auto publish = picomq.begin_publish(topic);
        serializeJson(json, publish);
        publish.send();

        Serial.print("Publishing readings: ");
        serializeJson(json, Serial);
        Serial.print("\n");
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

void update_status_led() {
    static bool wifi_was_connected = false;
    if (WiFi.status() == WL_CONNECTED) {
        {
            led_blinker.set_pattern(uint64_t(0b1) << 60);
        }
        if (!wifi_was_connected) {
            syslog.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
        }
        wifi_was_connected = true;
    } else {
        led_blinker.set_pattern(0b1100);
    }
    led_blinker.tick();
};

void loop() {
    ArduinoOTA.handle();

    update_status_led();
    server.handleClient();
    picomq.loop();

    {
        std::lock_guard<std::mutex> guard(mutex);
        publish_readings();
        HomeAssistant::tick();
    }
}
