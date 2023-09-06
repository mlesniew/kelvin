#pragma once

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

    protected:
        const String address;
        std::string name;
        mutable bool publish_pending;
        unsigned long timestamp;
        float temperature;
        float humidity;
        unsigned int battery;
};
