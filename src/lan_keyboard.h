#pragma once
#include "keyboard_bitmap.h"
#include <string>

namespace lan_keyboard {
void start(const std::string& configPath, void (*log)(const char*));
void stop();
dma_keyboard::Sample sample();
const char* status();
}
