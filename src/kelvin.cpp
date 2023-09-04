#include <mutex>
#include <map>

#include <Arduino.h>

#include <BLEDevice.h>
#include <BLEScan.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <uri/UriRegex.h>

#include <ArduinoJson.h>
#include <PicoMQTT.h>
#include <PicoUtils.h>
#include <WiFiManager.h>

PicoUtils::PinInput<0, true> button;
PicoUtils::PinOutput<2, false> wifi_led;
PicoUtils::Blink led_blinker(wifi_led, 0, 91);

String hostname;
String hass_autodiscovery_topic;

PicoUtils::RestfulServer<WebServer> server;
PicoMQTT::Client mqtt;

std::mutex mutex;

struct Reading {
    unsigned long timestamp;
    float temperature;
    float humidity;
    unsigned int battery;
};

std::map<BLEAddress, std::string> discovered;
std::map<BLEAddress, std::string> subscribed;
std::map<BLEAddress, Reading> readings;

void report_device(BLEAdvertisedDevice & device) {

    static const char ADDRESS_PREFIX[] = {0xa4, 0xc1, 0x38};
    static const BLEUUID THERMOMETHER_UUID{(uint16_t) 0x181a};

    auto address = device.getAddress();
    if (memcmp(address.getNative(), ADDRESS_PREFIX, 3) != 0) {
        return;
    }

    {
        // add device to discovered device list
        std::string & stored_name = discovered[address];

        // store name if it was captured
        const auto name = device.getName();
        if (name.size()) {
            stored_name = name;
        }
    }

    {
        // see if we're subscribed to the address
        auto it = subscribed.find(address);
        if (it == subscribed.end()) {
            return;
        }
    }

    Reading & reading = readings[address];

    for (int i = 0; i < device.getServiceDataUUIDCount(); ++i) {
        if (!device.getServiceDataUUID(i).equals(THERMOMETHER_UUID)) {
            continue;
        }

        const auto raw_data = device.getServiceData();

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

        if (sizeof(data) != raw_data.size()) {
            continue;
        }

        memcpy(&data, raw_data.c_str(), sizeof(data));

        reading.temperature = 0.01 * (float) data.temperature;
        reading.humidity = 0.01 * (float) data.humidity;
        reading.battery = data.battery_level;
        reading.timestamp = millis();
    }
}

class ScanCallbacks: public BLEAdvertisedDeviceCallbacks {
        void onResult(BLEAdvertisedDevice advertisedDevice) override {
            std::lock_guard<std::mutex> guard(mutex);
            report_device(advertisedDevice);
        }
} scan_callbacks;

namespace config {

const char CONFIG_PATH[] PROGMEM = "/config.json";

DynamicJsonDocument get() {
    DynamicJsonDocument json(1024);

    json["subscriptions"].to<JsonObject>();

    for (const auto & kv : subscribed) {
        auto address = kv.first;
        json["subscriptions"][address.toString()] = kv.second;
    }

    return json;
}

void set(const JsonDocument & config) {
    subscribed.clear();
    const auto subscription_config = config["subscriptions"].as<JsonObjectConst>();
    for (JsonPairConst kv : subscription_config) {
        const auto address = BLEAddress(kv.key().c_str());
        subscribed[address] = kv.value().as<const char *>();
    }
}

void load() {
    PicoUtils::JsonConfigFile<StaticJsonDocument<1024>> config(SPIFFS, FPSTR(CONFIG_PATH));
    set(config);
}

bool save() {
    auto file = SPIFFS.open(FPSTR(CONFIG_PATH), FILE_WRITE);
    if (!file) {
        return false;
    }

    const auto written = serializeJson(config::get(), file);
    Serial.printf("Bytes written to %s: %u\n", file.path(), written);

    file.close();
    return written > 0;
}

}

namespace network_config {

const char CONFIG_PATH[] PROGMEM = "/network.json";

void load() {
    PicoUtils::JsonConfigFile<StaticJsonDocument<256>> config(SPIFFS, FPSTR(CONFIG_PATH));
    hostname = config["hostname"] | "kelvin";
    hass_autodiscovery_topic = config["hass_autodiscovery_topic"] | "homeassistant";
    mqtt.host = config["mqtt"]["server"] | "calor.local";
    mqtt.port = config["mqtt"]["port"] | 1883;
    mqtt.username = config["mqtt"]["username"] | "kelvin";
    mqtt.password = config["mqtt"]["password"] | "harara";
}

void save() {
    auto file = SPIFFS.open(FPSTR(CONFIG_PATH), "w");
    if (file) {
        StaticJsonDocument<256> config;
        config["hostname"] = hostname;
        config["hass_autodiscovery_topic"] = hass_autodiscovery_topic;
        config["mqtt"]["host"] = mqtt.host;
        config["mqtt"]["port"] = mqtt.port;
        config["mqtt"]["username"] = mqtt.username;
        config["mqtt"]["password"] = mqtt.password;
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
    WiFiManagerParameter param_hass_topic("hass_autodiscovery_topic", "Home Assistant autodiscovery topic",
                                          hass_autodiscovery_topic.c_str(), 64);

    WiFiManager wifi_manager;

    wifi_manager.addParameter(&param_hostname);
    wifi_manager.addParameter(&param_mqtt_server);
    wifi_manager.addParameter(&param_mqtt_port);
    wifi_manager.addParameter(&param_mqtt_username);
    wifi_manager.addParameter(&param_mqtt_password);
    wifi_manager.addParameter(&param_hass_topic);

    wifi_manager.startConfigPortal("Kelvin");

    hostname = param_hostname.getValue();
    mqtt.host = param_mqtt_server.getValue();
    mqtt.port = String(param_mqtt_port.getValue()).toInt();
    mqtt.username = param_mqtt_username.getValue();
    mqtt.password = param_mqtt_password.getValue();
    hass_autodiscovery_topic = param_hass_topic.getValue();

    network_config::save();
}

void setup() {
    wifi_led.init();
    led_blinker.set_pattern(0b10);
    PicoUtils::BackgroundBlinker bb(led_blinker);

    button.init();

    Serial.begin(115200);

    SPIFFS.begin();
    network_config::load();
    config::load();

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
        auto & scan = *BLEDevice::getScan();
        scan.setActiveScan(false);
        scan.setInterval(100);
        scan.setWindow(99);

        scan.setAdvertisedDeviceCallbacks(
            &scan_callbacks,
            true /* allow duplicates */,
            true /* parse */);

        // scan forever
        scan.start(0, nullptr, false);
    }

    server.on("/readings", HTTP_GET, [] {
        std::lock_guard<std::mutex> guard(mutex);

        StaticJsonDocument<1024> json;

        for (const auto & kv : readings) {
            auto address = kv.first;
            const auto & reading = kv.second;

            const auto it = subscribed.find(address);
            if (it == subscribed.end()) {
                continue;
            }

            const auto name = it->second;

            auto json_element = json[name];
            json_element["temperature"] = reading.temperature;
            json_element["humidity"] = reading.humidity;
            json_element["battery"] = reading.battery;
            json_element["age"] = (millis() - reading.timestamp) / 1000;
        }

        server.sendJson(json);
    });

    server.on("/discovered", HTTP_GET, [] {
        std::lock_guard<std::mutex> guard(mutex);
        StaticJsonDocument<1024> json;
        for (const auto & kv : discovered) {
            auto address = kv.first;
            const auto & name = kv.second;
            json[address.toString()] = name;
        }
        server.sendJson(json);
    });

    server.on("/subscriptions", [] {
        std::lock_guard<std::mutex> guard(mutex);
        StaticJsonDocument<1024> json;
        for (const auto & kv : subscribed) {
            auto address = kv.first;
            const auto & name = kv.second;
            json[address.toString()] = name;
        }
        server.sendJson(json);
    });

    server.on("/config", HTTP_GET, [] {
        std::lock_guard<std::mutex> guard(mutex);
        server.sendJson(config::get());
    });

    server.on("/config/save", HTTP_POST, [] {
        std::lock_guard<std::mutex> guard(mutex);
        server.send(config::save() ? 200 : 500);
    });

    server.on(UriRegex("/subscriptions/((?:[a-fA-F0-9]{2}:){5}[a-fA-F0-9]{2})"), [] {
        std::lock_guard<std::mutex> guard(mutex);

        const std::string addrstr = server.pathArg(0).c_str();
        BLEAddress address{addrstr};

        switch (server.method()) {
            case HTTP_POST:
            case HTTP_PUT:
            case HTTP_PATCH:
                subscribed[address] = server.arg("plain").c_str();
            case HTTP_GET: {
                auto it = subscribed.find(address);
                if (it == subscribed.end()) {
                    server.send(404);
                } else {
                    server.send(200, F("text/plain"), it->second.c_str());
                }
            }
            return;
            case HTTP_DELETE:
                subscribed.erase(address);
                server.send(200);
                return;
            default:
                server.send(405);
                return;
        }
    });

    server.begin();
    mqtt.begin();
    led_blinker.set_pattern(1);
}

void publish_readings() {
    static unsigned long last_update = 0;

    std::lock_guard<std::mutex> guard(mutex);

    for (const auto & kv : readings) {
        auto address = kv.first;
        const auto & reading = kv.second;

        const auto it = subscribed.find(address);
        if (it == subscribed.end()) {
            // not subscribed (anymore...)
            continue;
        }

        if (reading.timestamp < last_update) {
            // already published
            continue;
        }

        const auto name = it->second;

        Serial.printf("%s  %-16s  %6.2f  %6.2f  %3u\n",
                      address.toString().c_str(),
                      name.c_str(),
                      reading.temperature,
                      reading.humidity,
                      reading.battery);

        const String topic = "calor/" + hostname + "/" + name.c_str();
        mqtt.publish(topic + "/temperature", String(reading.temperature));
        mqtt.publish(topic + "/humidity", String(reading.humidity));
        mqtt.publish(topic + "/battery", String(reading.battery));
    }

    last_update = millis();
}

PicoUtils::PeriodicRun hass_autodiscovery(300, 30, [] {
    if (hass_autodiscovery_topic.length() == 0) {
        return;
    }

    std::lock_guard<std::mutex> guard(mutex);

    Serial.println("Home assistant autodiscovery announcement...");

    const String mac(ESP.getEfuseMac(), HEX);

    struct Entity {
        String name;
        String icon;
        String device_class;
        String unit_of_measurement;
    };

    static const Entity entities[] = {
        {F("temperature"), F("mdi:thermometer"), F("temperature"), F("°C")},
        {F("humidity"), F("mdi:cloud-percent"), F("humidity"), F("%")},
        {F("battery"), F("mdi:battery"), F("battery"), F("%")},
    };

    for (const auto & kv : subscribed) {
        auto address = kv.first;
        auto discovered_it = discovered.find(address);
        if (discovered_it == discovered.end()) {
            continue;
        }
        auto device_name = discovered_it->second;

        auto device_id = String(address.toString().c_str());
        device_id.replace(":", "");

        for (const auto & entity : entities) {
            const auto unique_id = device_id + "-" + entity.name;
            const auto & name = kv.second;

            // TODO: should we drop hostname (node_id)?
            const String topic = hass_autodiscovery_topic + "/sensor/" + hostname + "/" + unique_id + "/config";

            StaticJsonDocument<1024> json;
            json[F("unique_id")] = unique_id;
            json[F("expire_after")] = 120;
            json[F("state_class")] = "measurement";

            json[F("state_topic")] = "calor/" + hostname + "/" + name.c_str() + "/" + entity.name;
            json[F("name")] = entity.name;
            json[F("icon")] = entity.icon;
            json[F("device_class")] = entity.device_class;
            json[F("unit_of_measurement")] = entity.unit_of_measurement;

            auto device = json[F("device")];
            device["name"] = device_name;
            device["suggested_area"] = name;
            device["identifiers"][0] = device_id;
            device["model"] = "Wireless temperature and humidity sensor";
            device["via_device"] = mac;

            auto publish = mqtt.begin_publish(topic, measureJson(json));
            serializeJson(json, publish);
            publish.send();
        }
    }
});

void update_status_led() {
    if (WiFi.status() == WL_CONNECTED) {
        if (mqtt.connected()) {
            led_blinker.set_pattern(uint64_t(0b101) << 60);
        } else {
            led_blinker.set_pattern(uint64_t(0b1) << 60);
        }
    } else {
        led_blinker.set_pattern(0b1100);
    }
    led_blinker.tick();
};

void loop() {
    update_status_led();
    server.handleClient();
    mqtt.loop();
    publish_readings();
    hass_autodiscovery.tick();
}
