#include "../src/webmap_state.h"

#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>

std::shared_ptr<const std::vector<radar_event::Marker>> eventScenario() {
    using namespace radar_event;
    Tracker tracker;
    std::vector<Object> objects;
    const Point observer{630, 1020};
    const auto add = [&](uint64_t first, Point pos) {
        for (uint64_t id = first; id < first + 8; ++id) objects.push_back({id, pos, {}});
    };
    add(1, {580, 960}); // Existing dense building.
    assert(tracker.update(1, 1, 1000, objects, observer, true).empty());
    assert(tracker.update(1, 1, 1100, objects, observer, true).empty());
    add(100, {570, 1020});
    add(200, {700, 1020});
    auto markers = tracker.update(1, 1, 1200, objects, observer, true);
    assert(markers.size() == 2);
    const auto drop = knownType("ExpansionAirdropContainer_Military");
    const auto wreck = knownType("Wreck_UH1Y");
    assert(drop == Kind::Airdrop && wreck == Kind::HelicopterCrash && !knownType("GenericAirdropBox"));
    objects.push_back({300, Point{568, 1022}, drop});
    objects.push_back({301, Point{820, 1020}, wreck});
    markers = tracker.update(1, 1, 1400, objects, observer, true);
    assert(markers.size() == 3 && markers[0].kind == Kind::Airdrop && markers[0].pos.x == 568);
    objects[24].pos = Point{570, 1020};
    markers = tracker.update(1, 1, 1500, objects, observer, true);
    assert(markers.size() == 3);
    assert(markers[0].kind == Kind::Airdrop && markers[0].pos.x == 570 && markers[0].pos.z == 1020);
    assert(markers[1].kind == Kind::HelicopterCrash && markers[1].pos.x == 820 && markers[1].pos.z == 1020);
    assert(markers[2].kind == Kind::LootCluster && markers[2].pos.x == 700 && markers[2].pos.z == 1020);
    assert(tracker.update(1, 1, 1500, objects, observer, true).size() == 3);
    Tracker changedMap = tracker, changedWorld = tracker;
    auto endedDrop = objects;
    endedDrop.erase(endedDrop.begin() + 24);
    Tracker expiredDrop = tracker;
    assert(expiredDrop.update(1, 1, 4000, endedDrop, observer, true).size() == 3);
    const auto remaining = expiredDrop.update(1, 1, 4501, endedDrop, observer, true);
    assert(remaining.size() == 2 && remaining[1].kind == Kind::LootCluster && remaining[1].pos.x == 700);
    auto stable = std::make_shared<const std::vector<Marker>>(std::move(markers));
    objects.resize(8);
    assert(changedMap.update(1, 2, 1600, objects, observer, true).empty());
    assert(changedWorld.update(2, 1, 1600, objects, observer, true).empty());
    assert(tracker.update(1, 1, 5001, objects, observer, true).empty());
    assert(tracker.update(1, 2, 5100, objects, observer, true).empty());
    assert(tracker.update(2, 2, 5200, objects, observer, true).empty());
    return stable;
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    webmap::Options options;
    options.enabled = true;
    options.port = std::stoi(argv[2]);
    options.hz = 20;
    options.release = webmap::CurrentMap | webmap::Own | webmap::Heading | webmap::Loot;
    options.publicHost = "webmap.example.com";
    webmap::configure(options, argv[1]);
    std::atomic<bool> stop{false}, friendMarked{true};
    std::atomic<unsigned> held{1};
    std::atomic<bool> eventStage{false}, differentMap{false};
    const auto events = eventScenario();
    std::thread writer([&] {
        unsigned generation = 0;
        while (!stop.load()) {
            const float x = float(++generation % 400 + 5);
            const bool alternate = differentMap.load();
            radar::Frame frame;
            frame.ok = true;
            frame.meX = x;
            frame.meZ = 2 * x;
            frame.fwdX = 1;
            frame.mapSize = alternate ? 8192.f : 15360.f;
            frame.meName = "private-name";
            frame.meHeld = "OwnRifle";
            if (eventStage && !alternate) frame.events = events;
            radar::Dot player;
            player.x = 500; player.z = 1000; player.fwdX = 1; player.name = "private-player";
            player.webTrackId = 42; player.webFriend = friendMarked.load();
            const auto weapon = held.load();
            player.held = weapon == 1 ? "GZ_TestRifle" : weapon == 2 ? "GZ_SecondRifle" : "";
            frame.dots.push_back(player);
            player.x = 600; player.fwdX = 0;
            player.webTrackId = 43; player.webFriend = false; player.name = "Second player";
            player.held.clear();
            frame.dots.push_back(player);
            radar::Dot loot;
            loot.icon = radar::Icon::Loot; loot.x = x; loot.z = 2 * x;
            loot.label = "private-item";
            frame.dots.push_back(loot);
            webmap::publish(alternate ? "different" : "chernarusplus", std::move(frame));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit") break;
        if (line == "unfriend") { friendMarked = false; continue; }
        if (line == "swap") { held = 2; continue; }
        if (line == "empty") { held = 0; continue; }
        if (line == "mapchange") { differentMap = true; continue; }
        if (line == "maphome") { differentMap = false; continue; }
        if (line == "events") eventStage = true;
        options.enabled = line != "off";
        options.release = line == "hide" ? 0 : webmap::CurrentMap | webmap::Own | webmap::Heading | webmap::Loot;
        if (line == "players" || line == "names" || line == "events" || line == "held") options.release |= webmap::Players;
        if (line == "names" || line == "events") options.release |= webmap::PlayerNames;
        if (line == "held" || line == "events") options.release |= webmap::PlayerHeld;
        if (line == "events") options.release |= webmap::Events;
        if (line == "guests") options.release = webmap::CurrentMap | webmap::Players | webmap::PlayerNames | webmap::PlayerHeld;
        webmap::configure(options, argv[1]);
    }
    stop = true;
    writer.join();
    webmap::shutdown();
    return 0;
}
