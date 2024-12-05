#pragma once

#include <map>

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>

class Names {
    public:
        Names(): dirty(false) {}

        JsonDocument json() const;

        void load();
        void save();

        const char * operator[](const BLEAddress & address) const;
        void set(const BLEAddress & address, const String & name);

        bool is_dirty() const { return dirty; }

    protected:
        std::map<BLEAddress, String> names;
        bool dirty;
};

