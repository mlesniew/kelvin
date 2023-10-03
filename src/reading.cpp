#include <BLEDevice.h>
#include <BLEScan.h>

#include <PicoSyslog.h>

#include "globals.h"
#include "reading.h"

extern PicoSyslog::SimpleLogger syslog;

namespace Metrics {
PicoPrometheus::Gauge temperature(prometheus, "air_temperature", "Air temperature in degrees Celsius");
PicoPrometheus::Gauge humidity(prometheus, "air_humidity", "Relative air humidity in percent");
PicoPrometheus::Gauge battery_level(prometheus, "battery_level", "Battery level in percent");
PicoPrometheus::Gauge battery_voltage(prometheus, "battery_voltage", "Battery voltage in volts");
PicoPrometheus::Histogram update_interval(prometheus, "update_interval", "Sensor beacon interval in seconds",
{1, 5, 10, 15, 30, 45, 60, 90, 120, 300});
}

Readings::Readings(const BluetoothDevice & sensor): sensor(sensor) {
    const auto labels = sensor.get_labels();
    Metrics::temperature[labels].bind(temperature);
    Metrics::humidity[labels].bind(humidity);
    Metrics::battery_level[labels].bind(battery_level);
    Metrics::battery_voltage[labels].bind(battery_voltage);
}

Readings::~Readings() {
    const auto labels = sensor.get_labels();
    Metrics::temperature.remove(labels);
    Metrics::humidity.remove(labels);
    Metrics::battery_level.remove(labels);
    Metrics::battery_voltage.remove(labels);
}

BluetoothDevice::BluetoothDevice(/* const */ BLEAddress & address) : address(address.toString().c_str()) {
}

PicoPrometheus::Labels BluetoothDevice::get_labels() const {
    return {{"name", name.c_str()}, {"address", address.c_str()}};
};

void BluetoothDevice::update(BLEAdvertisedDevice & device) {
    last_seen.reset();

    if (name.length() == 0) {
        if (!device.haveName()) {
            return;
        }
        name = device.getName().c_str();
        syslog.printf("New device detected: %s (%s)\n", this->address.c_str(), name.c_str());
    }

    for (int i = 0; i < device.getServiceDataUUIDCount(); ++i) {
        static const BLEUUID THERMOMETHER_UUID{(uint16_t) 0x181a};

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

        const bool first_reading = !readings;
        if (first_reading) {
            syslog.printf("First readings for %s (%s) received.\n", address.c_str(), name.c_str());
            readings.reset(new Readings(*this));
        }

        readings->temperature = 0.01 * (double) data.temperature;
        readings->humidity = 0.01 * (double) data.humidity;
        readings->battery_level = data.battery_level;
        readings->battery_voltage = 0.001 * (double) data.battery_mv;

        if (!first_reading) {
            Metrics::update_interval[get_labels()].observe(double(readings->last_update.elapsed()));
        }
        readings->last_update.reset();

        break;
    }
}

DynamicJsonDocument BluetoothDevice::get_json() const {
    DynamicJsonDocument json(256);
    json["name"] = name.length() ? name.c_str() : (char *) 0;
    if (readings) {
        json["temperature"] = readings->temperature;
        json["humidity"] = readings->humidity;
        json["battery"]["percentage"] = readings->battery_level;
        json["battery"]["voltage"] = readings->battery_voltage;
        json["last_update"] = readings->last_update.elapsed();
    }
    json["last_seen"] = last_seen.elapsed();
    return json;
}
