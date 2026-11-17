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

std::shared_ptr<const radar_corpse::Markers> corpseScenario() {
    using namespace radar_corpse;
    Tracker tracker;
    const radar_event::Point pos{625, 1050};
    tracker.begin(1, 1, 1000);
    tracker.observe(1, false, {}, 1000);
    tracker.observe(2, false, {}, 1000);
    tracker.observe(3, true, pos, 1000); // Already dead when first seen.
    assert(!tracker.snapshot());
    tracker.begin(1, 1, 1100); // Absent players do not generate deaths.
    assert(!tracker.snapshot());
    assert(tracker.observe(1, true, {}, 1200));
    tracker.observe(1, true, pos, 1250);
    auto marker = tracker.snapshot();
    assert(marker && marker->size() == 1 && marker->front().expiresAt == 1200 + kLifetimeMs);
    assert(marker->front().pos.x == pos.x && marker->front().pos.z == pos.z);
    tracker.observe(1, true, radar_event::Point{800, 1200}, 1400);
    assert(tracker.snapshot() == marker); // Frozen position and deadline.
    tracker.observe(1, false, {}, 1600); // Respawn at a reused address.
    tracker.observe(1, true, pos, 1700);
    assert(tracker.snapshot()->size() == 2 && tracker.snapshot()->back().id != marker->front().id);
    tracker.begin(1, 1, 1200 + kLifetimeMs - 1);
    assert(tracker.snapshot()->size() == 2);
    tracker.begin(1, 1, 1200 + kLifetimeMs);
    assert(tracker.snapshot()->size() == 1);
    tracker.begin(1, 2, 1200 + kLifetimeMs + 1);
    assert(!tracker.snapshot());
    tracker.observe(1, false, {}, 1200 + kLifetimeMs + 2);
    tracker.begin(2, 2, 1200 + kLifetimeMs + 3);
    tracker.observe(1, true, pos, 1200 + kLifetimeMs + 3);
    assert(!tracker.snapshot());
    tracker.clear();
    const uint64_t now = GetTickCount64();
    tracker.begin(2, 3, now);
    tracker.observe(10, false, {}, now);
    tracker.observe(10, true, pos, now);
    return tracker.snapshot();
}

int main(int argc, char** argv) {
    assert(loot::classify(loot::Other, "HuntingBag_Autumn") == loot::Bag);
    assert(loot::classify(loot::Other, "ExpansionCarKey_Red") == loot::VehicleKey);
    assert(loot::classify(loot::Other, "UndergroundStash") == loot::Stash);
    assert(loot::classify(loot::Other, "HiddenStash") == loot::Stash);
    for (const auto* name : {"GZ_Bitcoin", "Bitcoin", "gz_diamond", "GZEFT_LEDX", "Videocard_BL3080",
                             "GZEFT_HVGoldenPendant", "GZ_XM2010"})
        assert(loot::classify(loot::Other, name) == loot::HighValue);
    for (const auto* name : {"BitcoinMining_Kit", "GZBM_PowerSupplyUnit", "BBP_Carpet_DiamondPlate",
                             "CigarettePack_Merkur", "M4A1", "gz_diamond_fake"})
        assert(loot::classify(loot::Other, name) != loot::HighValue);
    assert(loot::mapVisible(false, 0, 0, loot::HighValue, "GZEFT_LEDX", true));
    assert(!loot::mapVisible(true, loot::kAll, 0, loot::HighValue, "GZEFT_LEDX", false));
    for (const auto* name : {"Motorbike_Red_Cover", "Motorbike_COVER", "OffroadHatchback_Blue_Cover",
                             "Truck_01_Cover", "ExpansionUAZ_Green_Cover", "GZ_Quad_Black_Cover"})
        assert(loot::classify(loot::Other, name) == loot::CoveredVehicle);
    for (const auto* name : {"Motorbike_Red", "Motorbike_Wheel_Cover", "Backpack_Cover", "Weapon_Cover",
                             "Truck_01_Covered", "Truck_01_Cover_Kit", "Helicopter_Cover", "Boat_Cover"})
        assert(loot::classify(loot::Other, name) != loot::CoveredVehicle);
    assert(loot::mapVisible(false, 1u << loot::CoveredVehicle, 0, loot::CoveredVehicle, "Truck_01_Cover"));
    assert(!loot::mapVisible(true, 1u << loot::Other, 0, loot::CoveredVehicle, "Truck_01_Cover"));
    assert(loot::classify(loot::Other, "Keycard_Red") != loot::VehicleKey);
    assert(loot::classify(loot::Other, "SeaChest") != loot::Stash);
    for (const auto category : {loot::Bag, loot::VehicleKey, loot::Stash})
        assert(loot::mapVisible(false, 0, loot::kStashLoot, category, ""));
    assert(!loot::mapVisible(false, loot::kAll, loot::kStashLoot, loot::Tool, "Hammer"));
    assert(!loot::mapVisible(false, loot::kAll, 0, loot::Bag, "HuntingBag_Autumn"));
    assert(!loot::mapVisible(true, loot::kAll, 1u << loot::VehicleKey, loot::Bag, "HuntingBag_Autumn"));
    assert(loot::mapVisible(true, loot::kAll, 1u << loot::VehicleKey, loot::VehicleKey, "ExpansionCarKey_Red"));
    if (argc != 3) return 2;
    const auto hp = player_health::parse("HIRSCHGANG-HP-1 1000 chernarusplus Private Test\n42 500 1000 37.5 50\n");
    assert(hp.current(3499, "chernarusplus", "Private Test"));
    assert(!hp.current(3500, "chernarusplus", "Private Test"));
    assert(!hp.current(1200, "enoch", "Private Test"));
    assert(!hp.current(1200, "chernarusplus", "Different Server"));
    assert(hp.find(42, 500, 1000) == 75 && hp.find(42, 551, 1000) == player_health::unknown);
    assert(hp.find(99, 500, 1000) == player_health::unknown);
    assert(player_health::label(0) == "HP 0" && player_health::label(100) == "HP 100");
    assert(player_health::label(player_health::unknown).empty());
    assert(player_health::parse("HIRSCHGANG-HP-1 1000 chernarusplus Test\n42 500 1000 10 0\n").at == 0);
    assert(player_health::parse("HIRSCHGANG-HP-1 1000 chernarusplus Test\n42 500 1000 10 50\n42 500 1000 20 50\n").at == 0);
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
    std::atomic<bool> backpackPresent{true};
    std::atomic<bool> playerVisible{true};
    std::atomic<unsigned> returningId{42};
    const auto events = eventScenario();
    const auto corpses = corpseScenario();
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
            frame.meHealth = 100;
            if (eventStage && !alternate) {
                frame.events = events;
                frame.corpses = corpses;
            }
            radar::Dot player;
            player.x = 500; player.z = 1000; player.fwdX = 1; player.name = "private-player";
            player.health = hp.find(42, 500, 1000);
            player.webTrackId = returningId.load(); player.webFriend = friendMarked.load();
            const auto weapon = held.load();
            player.held = weapon == 1 ? "GZ_TestRifle" : weapon == 2 ? "GZ_SecondRifle" : "";
            if (playerVisible) frame.dots.push_back(player);
            player.x = 600; player.fwdX = 0;
            player.webTrackId = 43; player.webFriend = false; player.name = "Second player";
            player.held.clear();
            frame.dots.push_back(player);
            radar::Dot loot;
            loot.icon = radar::Icon::Loot; loot.x = x; loot.z = 2 * x;
            loot.label = "private-item";
            frame.dots.push_back(loot);
            if (!alternate) {
                radar::Dot highValue;
                highValue.icon = radar::Icon::Loot;
                highValue.cat = loot::HighValue;
                highValue.x = 520; highValue.z = 940;
                highValue.label = "GZ_Bitcoin";
                frame.dots.push_back(highValue);
                highValue.x = 630; highValue.label = "GZEFT_LEDX";
                frame.dots.push_back(highValue);
                radar::Dot vehicle;
                vehicle.icon = radar::Icon::Vehicle; vehicle.x = 490; vehicle.z = 1080;
                vehicle.label = "TestCar";
                frame.dots.push_back(vehicle);
                vehicle.icon = radar::Icon::Loot; vehicle.cat = ::loot::CoveredVehicle;
                vehicle.x = 710; vehicle.label = "Motorbike_Red_Cover";
                frame.dots.push_back(vehicle);

                loot.webStashLoot = true;
                loot.z = 960;
                loot.x = 450;
                loot.cat = ::loot::Bag; loot.label = "HuntingBag_Autumn";
                if (backpackPresent) frame.dots.push_back(loot);
                loot.x = 560; loot.cat = ::loot::VehicleKey; loot.label = "ExpansionCarKey_Red";
                frame.dots.push_back(loot);
                loot.x = 670; loot.cat = ::loot::Stash; loot.label = "UndergroundStash";
                frame.dots.push_back(loot);
                loot.x = 720; loot.cat = ::loot::Tool; loot.label = "Hammer";
                frame.dots.push_back(loot);
                loot.webStashLoot = false;
                loot.x = 780; loot.cat = ::loot::Bag;
                frame.dots.push_back(loot);
            }
            webmap::publish(alternate ? "different" : "chernarusplus", std::move(frame));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit") break;
        if (line == "unfriend") { friendMarked = false; continue; }
        if (line == "player-away") { playerVisible = false; continue; }
        if (line == "player-return") { returningId = 84; playerVisible = true; continue; }
        if (line == "take-backpack") { backpackPresent = false; continue; }
        if (line == "swap") { held = 2; continue; }
        if (line == "empty") { held = 0; continue; }
        if (line == "mapchange") { differentMap = true; continue; }
        if (line == "maphome") { differentMap = false; continue; }
        if (line == "events") eventStage = true;
        options.enabled = line != "off";
        options.stashLootMask = line == "stash-keys" ? 1u << loot::VehicleKey : loot::kStashLoot;
        options.release = line == "hide" ? 0 : webmap::CurrentMap | webmap::Own | webmap::Heading | webmap::Loot;
        if (line == "players" || line == "names" || line == "events" || line == "held") options.release |= webmap::Players;
        if (line == "names" || line == "events") options.release |= webmap::PlayerNames;
        if (line == "held" || line == "events") options.release |= webmap::PlayerHeld;
        if (line == "events") options.release |= webmap::Events | webmap::Corpses | webmap::PlayerHealth | webmap::StashLoot | webmap::Vehicles | webmap::HighValues;
        if (line == "high-values-only") options.release = webmap::CurrentMap | webmap::HighValues;
        if (line == "vehicles-only") options.release = webmap::CurrentMap | webmap::Vehicles;
        if (line == "stash-only") options.release = webmap::CurrentMap | webmap::Own | webmap::StashLoot;
        if (line == "stash-keys") options.release |= webmap::StashLoot;
        if (line == "guests") options.release = webmap::CurrentMap | webmap::Players | webmap::PlayerNames | webmap::PlayerHeld;
        webmap::configure(options, argv[1]);
    }
    stop = true;
    writer.join();
    webmap::shutdown();
    return 0;
}
