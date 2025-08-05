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
#include "hass.h"
#include "readings.h"
#include "names.h"

PicoUtils::PinInput button(0, true);
PicoUtils::PinOutput wifi_led(2, false);

String hostname;
String ota_password;

PicoUtils::RestfulServer<WebServer> server;
PicoMQ picomq;
PicoMQTT::Client mqtt;
PicoSyslog::Logger syslog("kelvin");

std::map<BLEAddress, Readings> readings;

bool active_scan_enabled;
PicoUtils::Stopwatch active_scan_stopwatch;
PicoUtils::Stopwatch last_mqtt_reconnect;
PicoUtils::WiFiControlSmartConfig wifi_control(wifi_led);

Names names;
PicoUtils::Stopwatch last_name_save;

static const unsigned char ADDRESS_PREFIX[] = {0xa4, 0xc1, 0x38};

class ScanCallbacks: public BLEAdvertisedDeviceCallbacks {
    public:
        static bool active_scan_required;

        void onResult(BLEAdvertisedDevice advertisedDevice) override {
            std::lock_guard<std::mutex> guard(mutex);

            auto address = advertisedDevice.getAddress();
            if (memcmp(address.getNative(), ADDRESS_PREFIX, 3) != 0) {
                return;
            }

            const String address_str = BLEAddress(address).toString().c_str();

            bool have_name = names[address];
            if (!have_name && advertisedDevice.haveName()) {
                const String name = advertisedDevice.getName().c_str();
                if (name.length() > 0) {
                    syslog.printf("Assigning name %s to %s\n", name.c_str(), address_str.c_str());
                    names.set(address, name);
                    have_name = true;
                }
            }

            for (int i = 0; i < advertisedDevice.getServiceDataUUIDCount(); ++i) {
                static const BLEUUID THERMOMETHER_UUID{(uint16_t) 0x181a};

                if (!advertisedDevice.getServiceDataUUID(i).equals(THERMOMETHER_UUID)) {
                    continue;
                }

                const auto raw_data = advertisedDevice.getServiceData();

                struct {
                    // uint8_t     size;   // = 18
                    // uint8_t     uid;    // = 0x16, 16-bit UUID
                    // uint16_t    UUID;   // = 0x181A, GATT Service 0x181A Environmental Sensing
                    uint8_t     MAC[6]; // [0] - lo, .. [6] - hi digits
                    int16_t     temperature;    // x 0.01 degree
                    uint16_t    humidity;       // x 0.01 %
                    uint16_t    battery_mv;     // mV
                    uint8_t     battery_level;  // 0..100 %
                    uint8_t     counter;        // measurement count
                    uint8_t     flags;  // GPIO_TRG pin (marking "reset" on circuit board) flags:
                    // bit0: Reed Switch, input
                    // bit1: GPIO_TRG pin output value (pull Up/Down)
                    // bit2: Output GPIO_TRG pin is controlled according to the set parameters
                    // bit3: Temperature trigger event
                    // bit4: Humidity trigger event
                } __attribute__((packed)) data;

                if (sizeof(data) != raw_data.length()) {
                    continue;
                }

                memcpy(&data, raw_data.c_str(), sizeof(data));

                const double temperature = 0.01 * (double) data.temperature;
                const double humidity = 0.01 * (double) data.humidity;
                const unsigned int battery_level = data.battery_level;
                const double battery_voltage = 0.001 * (double) data.battery_mv;

                const bool first_reading = (readings.count(address) == 0);
                if (first_reading) {
                    syslog.printf("Got first reading from %s (%s)\n", address_str.c_str(),
                                  have_name ? names[address] : "<unknown>");
                } else {
                    Serial.printf("Got reading from %s (%s)\n", address_str.c_str(),
                                  have_name ? names[address] : "<unknown>");
                }

                if (!have_name && first_reading) {
                    active_scan_required = true;
                    syslog.println("Requesting active scan.");
                }

                readings[address] = Readings(temperature, humidity, battery_level, battery_voltage);

                break;
            }
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

JsonDocument get() {
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
    return config;
}

void save() {
    auto file = SPIFFS.open(FPSTR(CONFIG_PATH), "w");
    auto config = get();
    serializeJson(config, file);
    if (file) {
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
    Serial.begin(115200);
    Serial.print(
        R"(

88  dP 888888 88     Yb    dP 88 88b 88
88odP  88__   88      Yb  dP  88 88Yb88
88"Yb  88""   88  .o   YbdP   88 88 Y88
88  Yb 888888 88ood8    YP    88 88  Y8

)"
    "Kelvin " __DATE__ " " __TIME__ "\n"
);

    wifi_led.init();
    button.init();

    SPIFFS.begin();
    network_config::load();

    Serial.println("Configuration:");
    serializeJson(network_config::get(), Serial);

    names.load();

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

        for (auto & kv : readings) {
            const auto & address = kv.first;
            const String address_str = BLEAddress(address).toString().c_str();
            const auto & reading = kv.second;

            auto e = json[address_str].to<JsonObject>();
            e["temperature"] = reading.temperature;
            e["humidity"] = reading.humidity;
            e["battery"]["voltage"] = reading.battery_voltage;
            e["battery"]["level"] = reading.battery_level;
            e["name"] = names[address];
            e["age"] = reading.age.elapsed();
        }

        server.sendJson(json);
    });

    server.on("/devices", HTTP_GET, [] {
        std::lock_guard<std::mutex> guard(mutex);
        server.sendJson(names.json());
    });

    server.on("/devices", HTTP_DELETE, [] {
        std::lock_guard<std::mutex> guard(mutex);
        names.clear();
        syslog.println(F("Enabling active scan after dropping names."));
        active_scan_enabled = true;
        restart_scan();
        server.send(200, "text/plain", "OK");
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

    wifi_control.get_connectivity_level = [] {
        unsigned int ret = 1;
        if (mqtt.connected()) { ++ret; }
        if (HomeAssistant::connected()) { ++ret; }
        return ret;
    };
}

void publish_readings() {
    static PicoUtils::Stopwatch last_publish;

    bool got_all_names = true;

    const bool just_reconnected = last_publish.elapsed() >= last_mqtt_reconnect.elapsed();

    for (const auto & kv : readings) {
        auto address = kv.first;
        const auto & reading = kv.second;

        const bool already_published = (reading.age.elapsed() > last_publish.elapsed());
        const bool recent = (reading.age.elapsed() <= 120);

        const char * name = names[address];
        got_all_names = got_all_names && name;

        if (already_published && !(recent && just_reconnected)) {
            // already published and we haven't just reconnected
            continue;
        }

        static const String topic_prefix = "celsius/" + get_board_id() + "/";

        if (name) {
            picomq.publish(topic_prefix + name + "/temperature", reading.temperature);
            picomq.publish(topic_prefix + name + "/humidity", reading.humidity);
            mqtt.publish(topic_prefix + name + "/temperature", String(reading.temperature));
        }

        picomq.publish(topic_prefix + String(address.toString().c_str()) + "/temperature", reading.temperature);
        picomq.publish(topic_prefix + String(address.toString().c_str()) + "/humidity", reading.humidity);
        mqtt.publish(topic_prefix + String(address.toString().c_str()) + "/temperature", String(reading.temperature));
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

        if (!active_scan_enabled && names.is_dirty() && last_name_save.elapsed() >= 30 * 60) {
            names.save();
            last_name_save.reset();
        }
    }

    no_wifi_reset();
}
