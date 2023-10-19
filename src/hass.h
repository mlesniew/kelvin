#pragma once

#include <Arduino.h>
#include <PicoMQTT.h>

namespace HomeAssistant {

extern PicoMQTT::Client mqtt;
extern String autodiscovery_topic;

void init();
void tick();

}
