#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace loot {

enum Cat : int {
    Other,
    Gun,
    Ammo,
    Attachment,
    Melee,
    Explosive,
    Medical,
    Food,
    Drink,
    Tool,
    Clothing,
    Bag,
    VehiclePart,
    Base,
    Rare,
    Stash,
    VehicleKey,
    CoveredVehicle,
    HighValue,
    Count
};

constexpr unsigned kAll = (1u << Count) - 1;
constexpr unsigned kStashLoot = (1u << Bag) | (1u << Stash) | (1u << VehicleKey);

struct Info {
    const char* name;
    const char* id;
    COLORREF color;
};
const Info& info(int cat);

constexpr bool shown(unsigned mask, int cat) { return cat >= 0 && cat < Count && (mask >> cat & 1u) != 0; }

// Ground items can migrate between the loot and near/far object tables.
constexpr bool mapSourceCategory(int cat, bool stashes, bool covered, bool highValues) {
    return (stashes && shown(kStashLoot, cat)) ||
           (covered && cat == CoveredVehicle) || (highValues && cat == HighValue);
}

inline float effectiveRange(int cat, float configured) {
    return (shown(kStashLoot, cat) || cat == CoveredVehicle || cat == HighValue) && configured < 1000.f
        ? 1000.f : configured;
}

bool valuable(int cat);
bool bitcoin(const std::string& name);

int classify(int engineCat, const std::string& name);

size_t loadOverrides(const std::string& path);

enum FilterMode : int { FilterOff, FilterBlack, FilterWhite };

struct FilterEntry {
    std::string pat;
    bool on = true;
    bool operator==(const FilterEntry& o) const { return on == o.on && pat == o.pat; }
};

struct Filter {
    int mode = FilterOff;
    std::vector<FilterEntry> list;
    bool operator==(const Filter& o) const { return mode == o.mode && list == o.list; }
    bool operator!=(const Filter& o) const { return !(*this == o); }
};

bool globMatch(const std::string& pat, const std::string& name);

void setFilter(const Filter& f);
Filter filter();
bool listed(const std::string& name);

bool visible(unsigned mask, int cat, const std::string& name);
inline bool mapVisible(bool groundLoot, unsigned mask, unsigned stashLoot, int cat, const std::string& name, bool highValues = false) {
    if (cat == HighValue) return highValues;
    if (cat == CoveredVehicle) return shown(mask, cat);
    if (shown(kStashLoot, cat)) return shown(stashLoot, cat);
    return groundLoot && visible(mask, cat, name);
}

std::string serializeList(const std::vector<FilterEntry>& list);
std::vector<FilterEntry> parseList(const std::string& s);

}
