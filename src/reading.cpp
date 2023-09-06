#include <BLEDevice.h>
#include <BLEScan.h>
#include <PicoMQTT.h>

#include "reading.h"

Reading::Reading(/* const */ BLEAddress & address) : address(address.toString().c_str()) {
}

void Reading::update(BLEAdvertisedDevice & device) {
    static const BLEUUID THERMOMETHER_UUID{(uint16_t) 0x181a};

    if (device.haveName()) {
        name = device.getName();
    }

    rssi = device.getRSSI();

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

        temperature = 0.01 * (double) data.temperature;
        humidity = 0.01 * (double) data.humidity;
        battery = data.battery_level;
        voltage = 0.001 * (double) data.battery_mv;
        timestamp = millis();

        publish_pending = true;

        break;
    }
}

DynamicJsonDocument Reading::get_json() const {
    DynamicJsonDocument json(256);
    json["name"] = name.size() ? name.c_str() : (char *) 0;
    json["temperature"] = temperature;
    json["humidity"] = humidity;
    json["battery"]["percentage"] = battery;
    json["battery"]["voltage"] = voltage;
    json["age"] = (millis() - timestamp) / 1000;
    json["rssi"] = rssi;
    return json;
}

void Reading::publish(PicoMQTT::Publisher & mqtt, bool force) const {
    if (!(publish_pending || force)) {
        return;
    }

    static const String topic_prefix = "celsius/" + String((uint32_t)ESP.getEfuseMac(), HEX) + "/";
    const String topic = topic_prefix + address;
    const auto json = get_json();

    auto publish = mqtt.begin_publish(topic, measureJson(json));
    serializeJson(json, publish);
    publish.send();

    Serial.print("Publishing readings: ");
    serializeJson(json, Serial);
    Serial.print("\n");

    publish_pending = false;
}
