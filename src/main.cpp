#include "app/App.hpp"

// Deliberately thin. Everything with a deadline runs on its own FreeRTOS task;
// this loop only orchestrates.
App app;

void setup() {
    app.begin();
}

void loop() {
    app.loop();
}
