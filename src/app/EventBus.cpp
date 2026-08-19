#include "EventBus.hpp"

bool EventBus::begin(uint8_t depth) {
    queue_ = xQueueCreate(depth, sizeof(AppEvent));
    return queue_ != nullptr;
}

bool EventBus::post(const AppEvent& event, TickType_t wait) {
    if (!queue_) return false;
    if (xQueueSend(queue_, &event, wait) != pdTRUE) {
        ++dropped_;
        return false;
    }
    return true;
}

bool EventBus::poll(AppEvent& event) {
    return queue_ && xQueueReceive(queue_, &event, 0) == pdTRUE;
}
