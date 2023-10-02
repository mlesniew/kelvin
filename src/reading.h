#pragma once

#include <memory>

#include <PicoUtils.h>
#include <ArduinoJson.h>

class BLEAddress;
class BLEAdvertisedDevice;

class BluetoothDevice;

struct Readings {
    Readings(const BluetoothDevice & sensor);
    ~Readings();

    Readings(const Readings &) = delete;
    Readings & operator=(const Readings &) = delete;

    const BluetoothDevice & sensor;

    double temperature;
    double humidity;
    unsigned int battery_level;
    double battery_voltage;

    PicoUtils::Stopwatch last_update;
};

class BluetoothDevice {
    public:
        BluetoothDevice(/* const */ BLEAddress & address);

        BluetoothDevice(const BluetoothDevice &) = delete;
        BluetoothDevice & operator=(const BluetoothDevice &) = delete;

        void update(BLEAdvertisedDevice & device);
        PicoPrometheus::Labels get_labels() const;
        DynamicJsonDocument get_json() const;

        const Readings * get_readings() const { return readings.get(); }

        const String address;
        String name;

    protected:
        PicoUtils::Stopwatch last_seen;

        std::unique_ptr<Readings> readings;
};
