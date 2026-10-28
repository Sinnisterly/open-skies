#pragma once
#include "ui/Browser.h"
#include <string>

namespace os::features {

// Fills type, rarity and values of a grid entry from the item tables, and adds them to its text.
void Describe(const std::string& id, ui::BrowserEntry& e);

// Registers the Items tab spawner page. Worker thread, after the Locator is ready.
void InstallItemsPage();

// Game thread. Puts one of a product in the Exosuit, the way a claimed reward arrives.
bool GiveProduct(const std::string& id);

}
