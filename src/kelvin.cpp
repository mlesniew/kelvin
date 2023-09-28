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
#include <PicoMQTT.h>
#include <PicoPrometheus.h>
#include <PicoSyslog.h>
#include <PicoUtils.h>
#include <WiFiManager.h>

#include "globals.h"
#include "reading.h"

PicoUtils::PinInput<0, true> button;
PicoUtils::PinOutput<2, false> wifi_led;
PicoUtils::Blink led_blinker(wifi_led, 0, 91);

String hostname;
String ota_password;

PicoUtils::RestfulServer<WebServer> server;
PicoMQTT::Client mqtt;
PicoSyslog::Logger syslog("kelvin");

std::map<BLEAddress, Reading> readings;

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

            auto emplace_result = readings.emplace(address, address);
            auto & reading = emplace_result.first->second;
            const bool is_new_element = emplace_result.second;

            reading.update(advertisedDevice);

            active_scan_required = active_scan_required || (is_new_element && !reading.has_name());
        }
} scan_callbacks;

bool ScanCallbacks::active_scan_required;

namespace network_config {

const char CONFIG_PATH[] PROGMEM = "/network.json";

void load() {
    PicoUtils::JsonConfigFile<StaticJsonDocument<256>> config(SPIFFS, FPSTR(CONFIG_PATH));
    const String default_hostname = "kelvin-" + get_board_id();
    hostname = config["hostname"] | default_hostname;
    mqtt.host = config["mqtt"]["server"] | "calor.local";
    mqtt.port = config["mqtt"]["port"] | 1883;
    mqtt.username = config["mqtt"]["username"] | "kelvin";
    mqtt.password = config["mqtt"]["password"] | "harara";
    syslog.server = config["syslog"] | "192.168.1.100";
    syslog.host = hostname;
    ota_password = config["ota_password"] | "";
}

void save() {
    auto file = SPIFFS.open(FPSTR(CONFIG_PATH), "w");
    if (file) {
        StaticJsonDocument<256> config;
        config["hostname"] = hostname;
        config["mqtt"]["host"] = mqtt.host;
        config["mqtt"]["port"] = mqtt.port;
        config["mqtt"]["username"] = mqtt.username;
        config["mqtt"]["password"] = mqtt.password;
        config["syslog"] = syslog.server;
        config["ota_password"] = ota_password;
        serializeJson(config, file);
        file.close();
    }
}

}

void config_mode() {
    led_blinker.set_pattern(0b100100100 << 9);

    WiFiManagerParameter param_hostname("hostname", "Hostname", hostname.c_str(), 64);
    WiFiManagerParameter param_mqtt_server("mqtt_server", "MQTT Server", mqtt.host.c_str(), 64);
    WiFiManagerParameter param_mqtt_port("mqtt_port", "MQTT Port", String(mqtt.port).c_str(), 64);
    WiFiManagerParameter param_mqtt_username("mqtt_user", "MQTT Username", mqtt.username.c_str(), 64);
    WiFiManagerParameter param_mqtt_password("mqtt_pass", "MQTT Password", mqtt.password.c_str(), 64);
    WiFiManagerParameter param_syslog("syslog", "Syslog server", syslog.server.c_str(), 64);
    WiFiManagerParameter param_ota_password("ota_password", "OTA password", ota_password.c_str(), 64);

    WiFiManager wifi_manager;

    wifi_manager.addParameter(&param_hostname);
    wifi_manager.addParameter(&param_mqtt_server);
    wifi_manager.addParameter(&param_mqtt_port);
    wifi_manager.addParameter(&param_mqtt_username);
    wifi_manager.addParameter(&param_mqtt_password);
    wifi_manager.addParameter(&param_syslog);
    wifi_manager.addParameter(&param_ota_password);

    wifi_manager.startConfigPortal("Kelvin");

    hostname = param_hostname.getValue();

    hostname = param_hostname.getValue();
    mqtt.host = param_mqtt_server.getValue();
    mqtt.port = String(param_mqtt_port.getValue()).toInt();
    mqtt.username = param_mqtt_username.getValue();
    mqtt.password = param_mqtt_password.getValue();
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

    if (!MDNS.begin(hostname.c_str())) {
        Serial.println(F("MDNS init failed"));
    }

    {
        BLEDevice::init("");
        active_scan_enabled = false;
        restart_scan();
    }

    server.on("/readings", HTTP_GET, [] {
        std::lock_guard<std::mutex> guard(mutex);

        DynamicJsonDocument json(2048);

        for (const auto & kv : readings) {
            auto address = kv.first;
            const auto & reading = kv.second;
            json[address.toString()] = reading.get_json();
        }

        server.sendJson(json);
    });

    prometheus.labels["module"] = "kelvin";
    prometheus.labels["board"] = get_board_id();

    prometheus.register_metrics_endpoint(server);
    server.begin();
    mqtt.begin();

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
    std::lock_guard<std::mutex> guard(mutex);

    bool got_all_names = true;

    for (const auto & kv : readings) {
        const auto & reading = kv.second;
        if (reading.has_name()) {
            reading.publish(mqtt);
        } else {
            got_all_names = false;
        }
    }

    if (active_scan_enabled && (got_all_names || (active_scan_stopwatch.elapsed() >= 10))) {
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
        if (!wifi_was_connected) {
            syslog.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
        }
        if (mqtt.connected()) {
            led_blinker.set_pattern(uint64_t(0b1) << 60);
        }
        wifi_was_connected = true;
    } else {
        if (wifi_was_connected) {
            syslog.println("WiFi connection lost.");
        }
        led_blinker.set_pattern(0b1100);
        wifi_was_connected = false;
    }
    led_blinker.tick();
};

void check_mqtt() {
    static PicoUtils::Stopwatch connection_loss_time;
    if (mqtt.connected()) {
        connection_loss_time.reset();
    } else if (connection_loss_time.elapsed() >= 10 * 60) {
        syslog.println("Couldn't establish MQTT connection for too long.  Rebooting...");
        ESP.restart();
    }
}

void loop() {
    ArduinoOTA.handle();

    update_status_led();
    server.handleClient();
    mqtt.loop();
    check_mqtt();
    publish_readings();
}
