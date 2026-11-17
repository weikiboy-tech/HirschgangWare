#include "../src/process.h"
#include "../src/rtti.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
bool pointer(uint64_t address) { return address >= 0x10000 && address < 0x7FFFFFFF0000ull && !(address & 7); }
struct Table { uint64_t data, countAndCapacity; };
struct Fields { uint64_t type; uint8_t deathFlag; uint32_t candidate; };

bool fields(const Process& dma, uint64_t address, Fields& f) {
    return pointer(address) && dma.read(address, f.type) &&
           dma.read(address + 0x156, f.deathFlag) &&
           dma.read(address + 0x194, f.candidate);
}

// A controlled HP change can reject direct fields on this build. Matching
// numbers are evidence for further checks, not a production offset.
void compareLocalFields(const Process& dma, uint64_t entity, const Fields& initial, float before, float after) {
    constexpr size_t bytes = 0x4000;
    std::array<unsigned char, bytes> first{}, second{};
    if (!dma.read(entity, first.data(), DWORD(first.size())))
        throw std::runtime_error("Local object could not be read for the baseline");
    std::puts("Baseline recorded. Change only the test player's HP, then press Enter on the radar PC.");
    if (std::getchar() == EOF) throw std::runtime_error("Comparison cancelled");
    Fields current{};
    if (!fields(dma, entity, current) || current.type != initial.type || (current.deathFlag & 4) ||
        !dma.read(entity, second.data(), DWORD(second.size())))
        throw std::runtime_error("Player changed, died or became unreadable");
    unsigned matches = 0;
    for (size_t offset = 0; offset < bytes; offset += sizeof(float)) {
        float a = 0, b = 0;
        std::memcpy(&a, first.data() + offset, sizeof a);
        std::memcpy(&b, second.data() + offset, sizeof b);
        if (std::fabs(a - before) < 0.05f && std::fabs(b - after) < 0.05f) {
            ++matches;
            std::printf("Direct float candidate +0x%zX: %.3f -> %.3f\n", offset, a, b);
        }
    }
    std::printf("Matching direct float fields: %u. Quality field: %u -> %u.\n",
                matches, initial.candidate, current.candidate);
}
}

int main(int argc, char** argv) try {
    if (argc != 1 && argc != 3) {
        std::puts("Usage: dayz-health-probe.exe [known baseline HP] [known changed HP]");
        return 2;
    }
    auto process = makeDmaProcess("fpga", "", true);
    std::string detail;
    if (!process->attach("DayZ_x64.exe", detail)) throw std::runtime_error(detail);
    const auto& dma = *process;
    uint64_t world = 0, ownLink = 0, ownInner = 0;
    if (!dma.read(dma.module().base + 0x1834B80, world) || !pointer(world))
        throw std::runtime_error("World pointer unavailable");
    const bool haveLink = dma.read(world + 0x29B0, ownLink) && pointer(ownLink) &&
                          dma.read(ownLink + 8, ownInner) && pointer(ownInner);
    const uint64_t own = haveLink ? ownInner - 0xA8 : 0;
    Fields self{};
    const bool haveSelf = fields(dma, own, self) && pointer(self.type);
    if (haveSelf)
        std::printf("Own candidate at +0x194 (uint32): %u; dead=%u\n", self.candidate, (self.deathFlag >> 2) & 1);
    else
        std::puts("Local player fields unavailable");
    if (argc == 3) {
        if (!haveSelf) throw std::runtime_error("Controlled comparison needs the local player");
        const float before = std::stof(argv[1]), after = std::stof(argv[2]);
        if (!(before >= 0 && before <= 100 && after >= 0 && after <= 100 &&
              std::fabs(before - after) >= 1) || (self.deathFlag & 4))
            throw std::runtime_error("The comparison needs distinct known values for a living player");
        compareLocalFields(dma, own, self, before, after);
    }

    std::set<uint64_t> players;
    for (const uint32_t offset : {0xF70u, 0x10B8u}) {
        Table table{};
        if (!dma.read(world + offset, table) || !pointer(table.data)) continue;
        const uint32_t count = uint32_t(table.countAndCapacity);
        if (!count || count > 512) continue;
        std::vector<uint64_t> entities(count);
        if (!dma.read(table.data, entities.data(), count * 8)) continue;
        for (const auto entity : entities) if (entity != own && pointer(entity)) players.insert(entity);
    }
    Image image;
    std::vector<VTableInfo> types;
    if (!image.load(dma, detail) || !findVTables(image, "DayZPlayer", types, detail))
        throw std::runtime_error("DayZPlayer RTTI unavailable: " + detail);
    std::set<uint64_t> playerTypes;
    for (const auto& type : types) playerTypes.insert(image.base + type.rva);

    std::map<uint32_t, unsigned> alive, dead;
    for (const auto entity : players) {
        Fields f{};
        if (!fields(dma, entity, f) || !playerTypes.count(f.type)) continue;
        ++((f.deathFlag & 4) ? dead : alive)[f.candidate];
    }
    std::printf("Entity slots read: %zu; DayZPlayer types: %zu\n", players.size(), playerTypes.size());
    if (alive.empty() && dead.empty()) throw std::runtime_error("No readable DayZPlayer samples");
    for (const auto& [value, count] : alive) std::printf("Alive: raw %u -> %u matching player(s)\n", value, count);
    for (const auto& [value, count] : dead) std::printf("Dead: raw %u -> %u matching player(s)\n", value, count);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
