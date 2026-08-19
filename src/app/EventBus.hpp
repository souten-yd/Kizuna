#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "AppTypes.hpp"

// Single point where buttons, the IMU, the network and the audio pipeline all
// converge. Producers may be on either core; the consumer is always the
// Arduino loop task.
class EventBus {
public:
    bool begin(uint8_t depth = 24);
    bool post(const AppEvent& event, TickType_t wait = 0);
    bool poll(AppEvent& event);
    uint16_t dropped() const { return dropped_; }

private:
    QueueHandle_t queue_ = nullptr;
    volatile uint16_t dropped_ = 0;
};
