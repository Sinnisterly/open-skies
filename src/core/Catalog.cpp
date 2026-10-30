#include "core/Features.h"
#include "ui/Icons.h"

namespace os {

namespace {

Feature T(const char* id, const char* tab, const char* sub, const char* section)
{
    Feature f;
    f.id = id; f.tab = tab; f.sub = sub; f.section = section;
    return f;
}

Feature Toggle(const char* id, const char* tab, const char* sub, const char* section, std::vector<Slider> sliders = {})
{
    Feature f = T(id, tab, sub, section);
    f.sliders = std::move(sliders);
    return f;
}

Feature Number(const char* id, const char* tab, const char* sub, const char* section, int64_t max, std::vector<int64_t> adds)
{
    Feature f = T(id, tab, sub, section);
    f.kind = Kind::Number;
    f.numMax = max;
    f.quickAdds = std::move(adds);
    f.bindable = false;
    return f;
}

Feature Planned(Feature f)
{
    f.avail = Avail::Planned;
    f.bindable = false;
    return f;
}

}

void RegisterCatalog(Registry& r)
{
    r.AddTab({ "exosuit",   icon::Astronaut, { "survival", "movement", "jetpack" } });
    r.AddTab({ "multitool", icon::Tool,      { "mining", "weapons", "scanning" } });
    r.AddTab({ "starship",  icon::Rocket,    {} });
    r.AddTab({ "exocraft",  icon::Rover,     {} });
    r.AddTab({ "crafting",  icon::Hammer,    {} });
    r.AddTab({ "inventory", icon::Box,       {} });
    r.AddTab({ "storage",   icon::Dolly,     {} });
    r.AddTab({ "economy",   icon::Coins,     { "currency", "standing" } });
    r.AddTab({ "world",     icon::Planet,    {} });
    r.AddTab({ "effects",   icon::Wand,      {} });
    r.AddTab({ "blueprints", icon::Clone,    {} });
    r.AddTab({ "bases",     icon::Satellite, {} });
    r.AddTab({ "teleport",  icon::Pin,       {} });
    r.AddTab({ "scout",     icon::Binoculars, {} });
    r.AddTab({ "missions",  icon::Scroll,    {} });
    r.AddTab({ "crew",      icon::Users,     {} });
    r.AddTab({ "items",     icon::Cubes,     {} });
    r.AddTab({ "unlocks",   icon::Lock,      {} });
    r.AddTab({ "garage",    icon::Garage,    {} });
    r.AddTab({ "expeditions", icon::Flag,    {} });
    r.AddTab({ "tuner",     icon::Sliders,   {} });
    r.AddTab({ "hotkeys",   icon::Keyboard,  {} });
    r.AddTab({ "settings",  icon::Gear,      {} });

    // Exosuit
    r.Add(Toggle("exo.no_damage",      "exosuit", "survival", "protection"));
    r.Add(Toggle("exo.hazard",         "exosuit", "survival", "protection"));
    r.Add(Toggle("exo.health",         "exosuit", "survival", "protection"));
    r.Add(Toggle("exo.shield",         "exosuit", "survival", "protection"));
    r.Add(Toggle("exo.no_break",       "exosuit", "survival", "protection"));
    r.Add(Toggle("exo.life_support",   "exosuit", "survival", "protection"));
    r.Add(Toggle("exo.stamina",        "exosuit", "movement", "stamina"));
    r.Add(Toggle("move.run_speed",     "exosuit", "movement", "speed", { { "mult", 1.f, 5.f, 2.f } }));
    r.Add(Toggle("move.noclip",        "exosuit", "movement", "speed"));
    r.Add(Toggle("exo.jetpack",        "exosuit", "jetpack", "fuel"));
    r.Add(Toggle("exo.marine_jetpack", "exosuit", "jetpack", "fuel"));
    r.Add(Toggle("move.jetpack_power", "exosuit", "jetpack", "power",
        { { "lift", 1.f, 8.f, 3.f }, { "air", 1.f, 8.f, 3.f }, { "speed", 1.f, 40.f, 10.f } }));

    // Multi-Tool
    r.Add(Toggle("mt.instant_mining", "multitool", "mining", "mining"));
    r.Add(Toggle("mt.waste_guard",    "multitool", "mining", "mining"));
    r.Add(Toggle("mt.beam_heat",      "multitool", "mining", "mining"));
    r.Add(Toggle("mt.terrain",        "multitool", "mining", "terrain"));
    r.Add(Toggle("mt.one_shot",       "multitool", "weapons", "combat"));
    r.Add(Toggle("mt.boltcaster",     "multitool", "weapons", "ammo"));
    r.Add(Toggle("mt.charge",         "multitool", "weapons", "ammo"));
    r.Add(Toggle("mt.free_reload",    "multitool", "weapons", "ammo"));
    r.Add(Toggle("mt.javelin",        "multitool", "weapons", "ammo"));
    r.Add(Toggle("mt.scan_instant",   "multitool", "scanning", "analysis"));
    r.Add(Toggle("mt.scan_look",      "multitool", "scanning", "analysis"));
    r.Add(Toggle("mt.topo_scanner",   "multitool", "scanning", "scanners"));
    r.Add(Toggle("mt.runic_lens",     "multitool", "scanning", "scanners"));

    // Starship
    r.Add(Toggle("ship.tech_charge",  "starship", "", "systems"));
    r.Add(Toggle("ship.rockets",      "starship", "", "weapons"));
    r.Add(Toggle("ship.photon_heat",  "starship", "", "weapons"));
    r.Add(Toggle("ship.no_heat",      "starship", "", "weapons"));
    r.Add(Toggle("ship.shield",       "starship", "", "systems"));
    r.Add(Toggle("ship.hull",         "starship", "", "systems"));
    r.Add(Toggle("ship.engine_speed", "starship", "", "flight", { { "mult", 1.f, 20.f, 4.f } }));
    r.Add(Toggle("ship.pulse_speed",  "starship", "", "flight", { { "mult", 1.f, 30.f, 4.f } }));
    r.Add(Toggle("ship.hyperdrive",   "starship", "", "flight"));
    r.Add(Toggle("ship.core_swap",    "starship", "", "systems"));
    r.Add(Toggle("ship.land_anywhere", "starship", "", "flight"));

    // Exocraft
    r.Add(Toggle("vehicle.fuel",     "exocraft", "", "systems"));
    r.Add(Toggle("vehicle.boost",    "exocraft", "", "systems"));
    r.Add(Toggle("vehicle.nautilon", "exocraft", "", "systems"));
    r.Add(Toggle("vehicle.weapons", "exocraft", "", "weapons"));
    r.Add(Toggle("fleet.no_damage", "starship", "", "fleet"));
    r.Add(Toggle("fleet.max_stats", "starship", "", "fleet"));
    for (const char* id : { "fleet.finish", "fleet.repair" }) {
        Feature a = T(id, "starship", "", "fleet");
        a.kind = Kind::Action;
        a.bindable = false;
        r.Add(std::move(a));
    }

    // Crafting
    r.Add(Toggle("craft.free",       "crafting", "", "crafting"));
    r.Add(Toggle("craft.always",     "crafting", "", "crafting"));
    r.Add(Toggle("refiner.free",     "crafting", "", "refiner"));
    r.Add(Toggle("refiner.fuel",     "crafting", "", "refiner"));
    r.Add(Toggle("refiner.instant",  "crafting", "", "refiner"));
    r.Add(Toggle("build.free",       "crafting", "", "building"));
    r.Add(Toggle("build.settlement", "crafting", "", "building"));
    r.Add(Toggle("build.freestyle",  "crafting", "", "building"));
    r.Add(Toggle("build.upload_limit", "crafting", "", "building"));
    r.Add(Toggle("build.all_parts",  "crafting", "", "building"));
    r.Add(Toggle("build.corvette",   "crafting", "", "building", { { "limit", 1.f, 20.f, 5.f }, { "reach", 1.f, 5.f, 2.f } }));

    // Inventory
    r.Add(Toggle("inv.no_overload",  "inventory", "", "technology"));
    r.Add(Toggle("inv.tech_charge",  "inventory", "", "technology"));
    r.Add(Toggle("inv.tech_repair",  "inventory", "", "technology"));
    r.Add(Toggle("inv.free_fuel",    "inventory", "", "technology"));
    Feature last = Number("inv.last_moved", "inventory", "", "stacks", 9999999, { 100, 1000, 9999 });
    r.Add(std::move(last));
    r.Add(Toggle("inv.max_stacks",   "inventory", "", "stacks"));
    for (const char* id : { "inv.slots_suit", "inv.slots_tool", "inv.slots_ship", "inv.slots_freighter",
                            "inv.super_suit", "inv.super_tool", "inv.super_ship", "inv.super_freighter" }) {
        Feature slots = T(id, "inventory", "", "slots");
        slots.kind = Kind::Action;
        slots.bindable = false;
        r.Add(std::move(slots));
    }

    // Economy
    r.Add(Number("eco.units",       "economy", "currency", "wallet", 4294967295LL, { 1000000, 100000000 }));
    r.Add(Number("eco.nanites",     "economy", "currency", "wallet", 4294967295LL, { 1000, 100000 }));
    r.Add(Number("eco.quicksilver", "economy", "currency", "wallet", 4294967295LL, { 1000, 100000 }));
    r.Add(Toggle("eco.free_shop", "economy", "currency", "shops"));
    r.Add(Toggle("eco.gifts",     "economy", "currency", "shops"));
    for (const char* id : { "eco.gek", "eco.vykeen", "eco.korvax" })
        r.Add(Number(id, "economy", "standing", "races", 99, { 10 }));
    for (const char* id : { "eco.merchants", "eco.mercenaries", "eco.explorers", "eco.outlaws" })
        r.Add(Number(id, "economy", "standing", "guilds", 99, { 10 }));
    r.Add(Number("eco.local", "economy", "standing", "local", 99, { 10 }));

    // World
    r.Add(Toggle("world.interact",    "world", "", "interaction"));
    r.Add(Toggle("world.portal",      "world", "", "interaction"));
    r.Add(Toggle("world.spawn_class", "world", "", "spawner",
                 { { "cls", 0.f, 3.f, 3.f, "%.0f", { "cls.c", "cls.b", "cls.a", "cls.s" } } }));
    r.Add(Toggle("world.time",        "world", "", "time", { { "hour", 0.f, 24.f, 12.f, "%.1f h" } }));
    r.Add(Toggle("world.speed",  "world", "", "time", { { "mult", 0.1f, 3.f, 1.f } }));
    r.Add(Toggle("world.storms", "world", "", "weather"));
    {
        Slider type{ "type", 0.f, 16.f, 0.f, "%.0f" }, strength{ "intensity", 0.f, 1.f, 0.f, "%.0f" };
        for (int i = 0; i <= 16; ++i) type.choices.push_back("weather.t." + std::to_string(i));
        strength.choices = { "weather.i.0", "weather.i.1" };
        r.Add(Toggle("world.weather", "world", "", "weather", { type, strength }));
    }
    r.Add(Toggle("world.storm_force", "world", "", "weather"));
    // Effects tab hotkeys (the tab itself is a page; these show on the Hotkeys page)
    for (const char* id : { "fx.move_left", "fx.move_right", "fx.move_fwd", "fx.move_back", "fx.move_up", "fx.move_down", "fx.turn_left",
                            "fx.turn_right", "fx.tilt_up", "fx.tilt_down", "fx.roll_left", "fx.roll_right", "fx.to_me", "fx.sky_toggle" }) {
        Feature a = T(id, "effects", "", "placed");
        a.kind = Kind::Action;
        r.Add(std::move(a));
    }
    for (const char* id : { "bp.paste", "bp.undo" }) {
        Feature a = T(id, "blueprints", "", "paste");
        a.kind = Kind::Action;
        r.Add(std::move(a));
    }
    {
        Feature tame = T("world.tame", "world", "", "creatures");
        tame.kind = Kind::Action;
        r.Add(std::move(tame));
    }
    r.Add(Toggle("world.tame_sight", "world", "", "creatures"));
    r.Add(Toggle("world.no_pirates", "world", "", "encounters"));
    r.Add(Toggle("world.no_wanted",  "world", "", "encounters"));
    r.Add(Toggle("cust.exact_colours", "world", "", "looks"));

    // Tuner: game values as multipliers, two or three per group
    auto x = [](const char* id) { return Slider{ id, 1.f, 10.f, 2.f }; };
    r.Add(Toggle("tune.foot",          "tuner", "", "player", { { "speed", 1.f, 4.f, 1.5f } }));
    r.Add(Toggle("tune.swim",          "tuner", "", "player", { { "speed", 1.f, 5.f, 2.f } }));
    r.Add(Toggle("tune.stamina",       "tuner", "", "player", { x("mult") }));
    r.Add(Toggle("tune.jet_refill",    "tuner", "", "player", { x("mult") }));
    r.Add(Toggle("tune.shield",        "tuner", "", "player", { x("mult") }));
    r.Add(Toggle("tune.terrain",       "tuner", "", "player", { { "size", 0.25f, 8.f, 2.f, "%.2fx" }, { "range", 1.f, 10.f, 2.f } }));
    r.Add(Toggle("tune.mining",        "tuner", "", "player", { x("mult") }));
    r.Add(Toggle("tp.fly",             "exosuit", "jetpack", "flight", { { "speed", 1.f, 10.f, 3.f } }));
    r.Add(Toggle("tune.space",         "tuner", "", "ship", { { "speed", 1.f, 10.f, 2.f }, { "boost", 1.f, 10.f, 2.f } }));
    r.Add(Toggle("tune.pulse",         "tuner", "", "ship", { x("mult") }));
    r.Add(Toggle("tune.warp",          "tuner", "", "ship", { { "mult", 1.f, 5.f, 2.f } }));
    r.Add(Toggle("tune.ship_recharge", "tuner", "", "ship", { x("mult") }));
    r.Add(Toggle("tune.vehicle_fuel",  "tuner", "", "exocraft", { x("mult") }));
    r.Add(Toggle("tune.vehicle_boost", "tuner", "", "exocraft", { { "mult", 1.f, 4.f, 1.5f } }));
    r.Add(Toggle("tune.vehicle_laser", "tuner", "", "exocraft", { { "speed", 1.f, 5.f, 2.f }, { "radius", 1.f, 5.f, 2.f } }));
    r.Add(Toggle("tune.day",           "tuner", "", "world", { { "mult", 0.25f, 8.f, 2.f, "%.2fx" } }));
    r.Add(Toggle("tune.storm_freq",    "tuner", "", "world", { { "freq", 0.1f, 5.f, 0.5f, "%.2fx" }, { "length", 0.1f, 4.f, 1.f } }));
    r.Add(Toggle("tune.space_hazard",  "tuner", "", "world", { { "mult", 0.f, 2.f, 0.f } }));
}

}
