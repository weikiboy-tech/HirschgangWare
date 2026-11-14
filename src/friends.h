#pragma once
// Persistent "teammate / friend" list keyed by player name (case-insensitive).
// Marked friends are drawn GREEN on the ESP and on every radar.
// Shared by esp.cpp (coloring) and radar.cpp (middle-click toggling).
#include <string>

namespace friends {
bool contains(const std::string& name);
bool toggle(const std::string& name);  // flips state, saves to friends.txt, returns true if now a friend
void load();
void save();
}  // namespace friends
