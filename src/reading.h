#pragma once

#include <string>
#include <ArduinoJson.h>

class BLEAddress;
class BLEAdvertisedDevice;

namespace PicoMQTT {
class Publisher;
};

class Reading {
    public:
        Reading(/* const */ BLEAddress & address);

        void update(BLEAdvertisedDevice & device);

        DynamicJsonDocument get_json() const;
        void publish(PicoMQTT::Publisher & mqtt, bool force = false) const;

        bool has_name() const { return name.size() > 0; }

    protected:
        const String address;
        std::string name;
        mutable bool publish_pending;
        unsigned long timestamp;
        double temperature;
        double humidity;
        unsigned int battery;
        int rssi;
        double voltage;
};
