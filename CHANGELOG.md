# Changelog

## 0.9.6

### Garage
- Fixed: editing your character (or any look) crashed or did nothing after the Forsaken update. A game function Garage uses before applying a look had moved, and the menu was calling the wrong one.
- Fixed: opening Character could freeze the game. The preview tried to draw your character before your look was read; it now waits for it.
- Fixed: the preview could get stuck on the first item it showed after one of the crashes above.

## 0.9.5

### Forsaken update
- Works with the Forsaken update (NMS 183791). Every hook and address the update moved has been found again.
- More of the menu now reads its addresses from the game itself, so future updates should break less.
- Items, unlocks and rewards are read from the game's own tables, so the new Forsaken items, corvette modules, titles and helmets show up in Items and Unlocks.

### Garage
- Fixed: a ship whose model was changed (for example to a Sentinel Interceptor) lost its installed tech after a reload.
- Fixed: changing a ship's class rolled its stats from the wrong ship type, so a Dropship could get Freighter stats.
- New: Tech rules for each ship: Normal, Living or Sentinel. They decide which tech the game keeps on a reload. A ship follows its model's rules, or pick others to keep normal tech on a Sentinel-look ship.
- Changing a ship's model or rules swaps installed tech to the matching version (normal, Living or Sentinel). A message lists what was swapped and anything that has no matching version.

### Inventory
- New: Supercharge Exosuit Tech, Supercharge Multi-Tool, Supercharge Starship Tech and Supercharge Freighter Tech. Every unlocked slot of that inventory becomes supercharged. Use Max Slots first for the full grid.

### Scout
- Fixed: scanning nearby systems found nothing after the update.
- Fixed: the current system showed "Data Unavailable" for economy and conflict when Scout was opened while the save was still loading.

### Missions
- Fixed: the Active list was empty after the update.

### Teleport
- Fixed: the current address and warp arrival after the update.

### Other fixes
- Faction standings can be set again.
- Placing a base part finds your nearest base again.
- Expeditions load with the larger season data in this update.
- The log now records where a failed action went wrong.

## 0.9.0

### New: Effects tab
- Browse all 893 of the game's own visual effects, grouped into fireworks, weather, warp and portals, fire, toxic, water, sparks, smoke, explosions, creatures, and trails and engines. Search, sort by name, group or game table, and keep favourites at the top.
- Test effect: the menu steps aside, the effect plays in front of you for as long as you choose, then the menu comes back.
- Place effects to decorate a base. Loop plays one again as soon as it ends; turn Loop off to play it on a timer instead. Placed effects come back every time you visit that system. They are kept by the menu, not your save, so only you see them.
- Placed effects are listed at the top of the tab. Pick one to move it, turn, tilt or roll it, bring it to you, or remove it. Every one of these is also a hotkey that works with the menu closed.
- Choose how many placed effects show at once. Past 150, the menu warns that the game's own effects nearby may stop showing.

### New: Custom weather
- Mix any effects into your own weather: lightning, meteors, tornadoes, dust devils, fire, fireworks and more go off at random around you wherever you go.
- Set how far they spread, how many per second, how high, how long each lasts, and whether they face random directions.
- Set the real weather underneath and force storms from the same page.
- Save mixes as presets, or press Create random weather for a surprise mix that saves itself.
- A hotkey turns custom weather on and off.

### World
- New: Force Weather. Every planet in the system gets the weather you pick (any of 17 types, named by their storms), at normal or extreme intensity. Turn it off to get the real weather back.
- New: Force Storms. Storms come as often as the game allows.

### Scout
- Sorting now fits the tab: nearest first, rarest first, name or star colour.

### Items
- Fixed: placing a base part put it behind you instead of in front.

### Menu
- The footer is larger and easier to read.

## 0.8.0

### New: Storage tab
- Every container you own in one place: Exosuit, each ship, freighter, storage containers 0 to 9, exocraft, and the smaller stores (ingredients, rocket locker, fishing, food unit, corvette parts).
- Find an item: search shows every container that holds it and how many.
- Send items between containers from any distance. Pick the item, where to take it from, how many, and where to send it. Part stacks fill first, and stack sizes follow the target container.
- Move everything out of a container in one go.
- Sort a container, or all of them, by value, type, name or colour with the game's own sort. Part stacks merge as it sorts.
- Item tiles show stack counts like the game's inventory.

### New: Scout tab
- This system: a report on the system you are in and every planet in it. It shows the economy, wealth, conflict, race and star colour, and for each planet the biome, weather, sentinels, resources, flora and fauna, in the game's own words.
- Nearby: scan the regions around you for real systems, using the game's own galaxy generator. Filter by black hole, Atlas station, star colour, race, economy, wealth, conflict, planet count and planet kind.
- Planets of systems you have never visited, worked out by the game's own planet generator.
- Sort by rarity. Rare systems get a gold star, based on how rare each trait is in what you scanned: purple stars, giant and exotic worlds, infested worlds, black holes and more.
- Hold to warp to a system, copy its portal address, or save it as a Teleport spot.

### Teleport
- Typed addresses now land on a real star. If there is no star at the address, you warp to a real system in the same region and a message tells you.
- The galaxy map moves to you after every warp.
- New Fix galaxy map button. It moves the map to you, and if you are stuck in a system with no star (from older builds), it warps you to the nearest real one.
- Spots saved from Scout keep only the system. They warp like a typed address, and the spot shows "You are here" when you are in that system.

### Unlocks
- Fixed: unlocking a ship gave an item with no use. Ships, Multi-Tools, frigates and pet eggs from expedition, Twitch and platform rewards now arrive the way the game hands them out, as the real ship, Multi-Tool, frigate or egg.

## 0.7.0

### Teleport
- Name a spot before saving it. Rename spots, move a spot to where you stand, and copy a spot's portal address. Spot names no longer repeat.
- Fixed: one saved spot stayed selected. Saving now selects the new spot, and clicking a selected spot again lets it go.
- Warp to any system in any galaxy through the game's own teleporter. Enter portal glyphs on the glyph pad, type galactic coordinates, pick a galaxy, or paste a copied address.
- The glyph pad shows the game's glyph icons, hex digits, or both.
- Spots saved in other systems warp you there, then put you on the spot.
- Warping works from inside your ship too. You arrive in your ship, including in other galaxies.

### Items and Unlocks
- Sort by name, type, base value, value in the current system, or rarity. The grid can be grouped by type.
- The inspector shows each item's type, rarity, base value, and its price in the current system with the difference from base.
- Utopia stations show under Exclusives.
- Spawning into a full inventory now says it failed.

### Reputation
- Standings show as soon as a save loads, and changes go through the game's own standing system, so ranks and titles update at once.
- New "This System" standing: your reputation with the race of the system you are in.

### World
- New toggle: No Pirate Encounters.
- New toggle: No Wanted Level. Sentinels never raise your wanted level, and turning it on clears the current one.

### Garage
- New live 3D preview of the selected ship, Multi-Tool, freighter, exocraft or character, rendered by the game itself with the item's parts and colours. Drag to turn it, scroll to zoom, and turn on a slow spin if you like. It stays visible on every Garage tab.
- Preview seed: see a seed on an owned item before you apply it.
- Add to Garage: pick a ship or Multi-Tool type, paste a community seed or roll one, preview it, find a seed with the parts you want, set class and name, then add it to a free slot in your save.
- The All tab lists only what your loaded save owns, with your ship and Multi-Tool slot counts. The full model catalogue is under Models.
- Wider side panel and a taller menu, so the editor fits without scrolling.
- Paint works on ships and Multi-Tools that used the old seed colours, and those colours now save. Ship types that use other colour groups get them.
- Keep Custom Colours applies to your current ship and Multi-Tool as soon as it is turned on.
- A ship that is still rebuilding gets its new look as soon as it can, instead of dropping it.

## 0.6.0
- Fixed: Unlimited Charge could leave the Launch Thruster reading 100% but unable to launch or refuel. Toggling it on or off now repairs broken charge.
- Fixed: tech spawned from the Items tab can now be charged.
- Unlocking a ship, Multi-Tool or other reward now gives you its item, so it can be claimed and summoned from the quick menu.
- New "Receive if already owned" switch in Unlocks gives a reward's item again.
- The item spawner has its own search field.

## 0.5.0
- Garage: parts by seed. Pick the parts you want and the menu finds a seed that grows them, through the game's own part roll. It is the Parts tab for living, exotic and Sentinel ships, and an extra section for other ships, Multi-Tools and freighters.
- Freestyle Building places anywhere: in the air, overlapping other parts, outside base range and in protected areas.
- All Building Parts follows the Beyond Base Building rules: every part in the menu with no blueprints, buildable on planets, freighters, stations and corvettes, with no build limits.
- Land Anywhere works in space, high in the atmosphere and on steep or blocked ground. Summoned and NPC ships still use the normal landing checks.
- Free Shops also covers nanites, quicksilver, cost and interaction prices.
- Infinite Jetpack covers boosts and launches; Infinite Stamina covers every stamina drain.
- No Technology Overload no longer removes adjacency bonuses or breaks "install an upgrade" mission checks.
- Skip intro logos setting, on by default.
- Save backups find GOG saves too.
- Fixed a long lag after loading a save, caused by the terrain manipulator size setting enlarging old terrain edits.

## 0.4.0
- Fly Mode is a real hover flight on foot: WASD follows the camera, Space rises, Ctrl sinks, Shift is faster. No-Clip now flies the same way instead of dropping you through the ground.
- Fixed being launched into space when Fly Mode and Jetpack Power were both on. Tuner features that share a game value now stack once instead of compounding every second.
- Fixed the mouse cursor vanishing when the menu scale is below 1.0x.
- Garage: parts and paint go through the game's own appearance code, so colours stick and a colour change keeps your parts. Paint offers each item's real 64-colour palette per colour group.
- Garage: Character customisation (race, head, torso, armour, gloves, legs, boots, backpack, cape), with locked parts allowed. Exocraft, staffs and ships use the same part picker.
- Garage: stat edits apply when you let go of the bar, and the log records how many values were written.
- New Creatures section on the World tab: Tame Creature in View and Tame on Sight, using the game's own adoption. Bindable on the Hotkeys page.
- Only Show One Class is now a class dropdown (C, B, A, S).
- Unlocks: new Exclusives chip with the expedition and pre-order techs (Waveform Engine, Frameshift Catapult, Advanced Launch System, Photonix Core, Boltcaster SM, Plasma Resonator, Visceral Synthesiser, Archive Bypass Implant) and the Utopia stations. New Swappable Ship Engines toggle so the upgrades can replace the core engines.
- Unlocks: fixed Twitch and platform reward grants writing to memory with the wrong arguments.
- Missions: stages advance and finish through the game's own mission update, so stage rewards and completion unlocks are given. Hints, trackers, guides and other entries that are not real missions are no longer listed, and table names are corrected.
- Expeditions: each past expedition picks its own season file instead of the first file with any matching number.
- Garage Paint: free colour picker with brightness, exact swatch colours, and Keep Custom Colours so they survive reloads.
- Garage stat edits now update the real ship and Multi-Tool stats.
- Missions: hint and tracker entries are hidden from Active too, and Finish with rewards no longer gets stuck.
- Freestyle Building: place parts with no range, limit, collision, snap, terrain, water or settlement checks.
- All Building Parts: every part with a menu group shows and can be placed anywhere.
- Bigger Base Uploads: 10000 parts instead of 3000.
- Terrain Manipulator Size: radius and beam range sliders in the Tuner.
- Land Anywhere: the ship lands even where the game says it cannot.
- Max inventory slots now use the right class for Exosuit and Multi-Tool, and refresh the inventory after adding slots.

## 0.3.0
- New tabs: Items spawner, Unlocks, Garage (live ship and Multi-Tool swaps), Expeditions (past expeditions and their rewards) and Teleport (saved spots).
- Weapons: unlimited clip and instant charge for every weapon, free reloads, no overheat for ship and Multi-Tool, frigates never damaged.
- Survival: health and shield never drop, tech never breaks, tech kept charged and repaired, life support, terrain manipulator and hyperdrive kept full.
- World: instant portals, locked time of day, game speed, no storms, free flight.
- Building: instant settlement construction, higher corvette and base limits.
- Inventory: keep stacks full, unlock every exosuit slot.
- Tuner: 15 groups of game values as multipliers, re-applied when the game reloads them.
- Save backup before the first save change of each session.
- Expedition season files from cwmonkey/nms-expeditions ship in `game/expeditions` and install next to the DLL.

## 0.2.0
- Game hooks for all 33 Cheat Engine scripts. Targets are found by strings, RTTI and call hops at startup, then checked before anything is patched.
- A feature whose target is missing shows as Unavailable with the reason. Everything else keeps working.
- Jetpack power and pulse speed edit the game's globals tables by field name.
- Tab strip shows arrows and one dot per category.
- `OpenSkiesTargetTest.exe` checks every target against NMS.exe without starting the game.

## 0.1.0
- First shell: version.dll proxy, Vulkan overlay, menu styled after the game's own UI, hotkeys, settings, logging and language packs.
- Feature catalog for every Cheat Engine table entry plus planned expansions. Game hooks arrive in the next pass.
