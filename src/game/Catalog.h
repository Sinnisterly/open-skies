#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace os::game {

enum class ItemKind : uint8_t { Substance, Product, Technology, BasePart, Customisation };

// One entry from the game's item tables, with localized text.
struct Item
{
    std::string id, name, desc, icon;
    ItemKind kind = ItemKind::Product;
    int type = -1, category = -1, rarity = -1; // raw enum values from the table
    int value = 0;
    bool procedural = false;
};

namespace catalog {

void Request();  // builds on the game thread; later calls rebuild (for example after a language change)
bool Ready();
bool Building();
std::shared_ptr<const std::vector<Item>> Items(); // never null once Ready()
const Item* Find(const std::string& id);          // pointers stay valid for the session

// Localized text for a key. Game thread only.
std::string Translate(const std::string& key);

// Price of an item in the current star system, -1 when no pricer is set or it has no price.
double LocalValue(const Item& it);
void SetLocalPricer(std::function<double(const Item&)> fn);

}

namespace prices {

void Install();     // worker thread: fills catalog::LocalValue for the current system
uint64_t Version(); // changes each time the prices are refilled

}

}
