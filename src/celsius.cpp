#include <mutex>
#include <map>

#include <Arduino.h>

#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <ESPmDNS.h>

bool scanning = false;
std::mutex mutex;

struct Readings {
    std::string name;
    float temperature;
    float humidity;
    unsigned int battery;
};

std::map<BLEAddress, Readings> readings;

void report_device(BLEAdvertisedDevice & device) {

    static const char ADDRESS_PREFIX[] = {0xa4, 0xc1, 0x38};
    static const BLEUUID THERMOMETHER_UUID{(uint16_t) 0x181a};

    auto address = device.getAddress();
    if (memcmp(address.getNative(), ADDRESS_PREFIX, 3) != 0) {
        return;
    }

    for (int i = 0; i < device.getServiceDataUUIDCount(); ++i) {
        if (!device.getServiceDataUUID(i).equals(THERMOMETHER_UUID)) {
            continue;
        }

        const auto raw_data = device.getServiceData();
        const auto name = device.getName();

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

        Serial.printf("%s  %-16s  %6.2f  %6.2f  %3u\n",
                      address.toString().c_str(),
                      name.c_str(),
                      0.01 * (float)data.temperature,
                      0.01 * (float)data.humidity,
                      data.battery_level);

        Readings & r = readings[address];
        r.temperature = 0.01 * (float) data.temperature;
        r.humidity = 0.01 * (float) data.humidity;
        r.battery = data.battery_level;

        if (name.size()) {
            r.name = name;
        }
    }
}

void scan_complete(BLEScanResults results) {
    std::lock_guard<std::mutex> guard(mutex);
    for (int i = 0; i < results.getCount(); ++i) {
        auto device = results.getDevice(i);
        report_device(device);
    }
    scanning = false;
}

void setup() {
    Serial.begin(115200);

    WiFi.hostname("celsius2");
    WiFi.setAutoReconnect(true);

    if (false) {
        // use smart config
        Serial.println("Beginning smart config...");
        WiFi.beginSmartConfig();

        while (!WiFi.smartConfigDone()) {
            delay(500);
            Serial.print(".");
        }
        Serial.println("Smart config complete");
    } else {
        // use stored credentials
        WiFi.softAPdisconnect(true);
        WiFi.begin();
    }

    if (MDNS.begin("celsius2")) {
        Serial.println(F("MDNS init failed"));
    }

    BLEDevice::init("");
    auto * scan = BLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
}

void loop() {
    if (!scanning) {
        scanning = BLEDevice::getScan()->start(10, scan_complete, false);
    }
}
