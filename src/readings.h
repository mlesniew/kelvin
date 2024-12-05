#pragma once

struct Readings {
    Readings(const double temperature, const double humidity, const unsigned int battery_level,
             const double battery_voltage):
        temperature(temperature), humidity(humidity), battery_level(battery_level), battery_voltage(battery_voltage) {
    }

    Readings(): Readings(
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN(),
            -1,
            std::numeric_limits<double>::quiet_NaN()) {}

    double temperature;
    double humidity;
    unsigned int battery_level;
    double battery_voltage;
    PicoUtils::Stopwatch age;
};
