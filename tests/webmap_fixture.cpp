#include "../src/webmap_state.h"

#include <atomic>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    webmap::Options options;
    options.enabled = true;
    options.port = std::stoi(argv[2]);
    options.hz = 20;
    options.release = webmap::CurrentMap | webmap::Own | webmap::Heading | webmap::Loot;
    options.publicHost = "webmap.example.com";
    webmap::configure(options, argv[1]);
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        unsigned generation = 0;
        while (!stop.load()) {
            const float x = float(++generation % 400 + 5);
            radar::Frame frame;
            frame.ok = true;
            frame.meX = x;
            frame.meZ = 2 * x;
            frame.fwdX = 1;
            frame.mapSize = 15360;
            frame.meName = "private-name";
            radar::Dot player;
            player.x = 500; player.z = 1000; player.fwdX = 1; player.name = "private-player";
            frame.dots.push_back(player);
            player.x = 600; player.fwdX = 0;
            frame.dots.push_back(player);
            radar::Dot loot;
            loot.icon = radar::Icon::Loot; loot.x = x; loot.z = 2 * x;
            loot.label = "private-item";
            frame.dots.push_back(loot);
            webmap::publish("chernarusplus", std::move(frame));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit") break;
        options.enabled = line != "off";
        options.release = line == "hide" ? 0 : webmap::CurrentMap | webmap::Own | webmap::Heading | webmap::Loot;
        if (line == "players") options.release |= webmap::Players;
        webmap::configure(options, argv[1]);
    }
    stop = true;
    writer.join();
    webmap::shutdown();
    return 0;
}
