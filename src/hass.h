#pragma once

#include <PicoMQTT.h>

namespace HomeAssistant {

extern PicoMQTT::Client mqtt;

void init();
void tick();

}
