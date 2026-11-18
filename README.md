# Fortress Forever NextBot

FF NextBot is a Metamod:Source plugin that adds bots to **Fortress Forever 2013** dedicated servers.
The bots run Valve's NextBot / TFBot AI and its nav mesh generator. An FF game layer adapts them to
FF's classes, weapons, four teams, Lua alliances and Lua-defined game modes.

It is a sibling of [prefortress2-nextbot](https://github.com/AgitationSkeleton/prefortress2-nextbot)
and shares its core.

## What the bots do

| Mode (map Lua) | Behaviour |
|---|---|
| Capture the flag (`base_ctf`, `default.lua`) | Steal the enemy flag and deliver it to their own cap. Escort carriers. About 30% of each team defends its flag (`ff_bot_ctf_defense_ratio`). |
| Attack/defend, invade/defend (`base_ad`, `base_id`, zone variants) | Attackers carry the current phase's flag to the cap that accepts it. Defenders guard that cap. Phases follow the map Lua (removed/restored objectives). |
| Command points (`base_cp`) | Take the nearest zone their side doesn't own, otherwise hold the owned zone nearest the front. |
| Ball (`base_fortball`) | Treat the ball as a neutral flag and carry it to their goal. |
| Hunted (`base_hunted`) | One Civilian VIP heads for the escape. Allied bodyguards climb up and press the escape-door buttons, then escort. Assassins hunt the VIP. |
| Deathmatch / teamplay / anything else | Roam and fight (TFBot SeekAndDestroy), across up to four teams with FFA and ally rules respected. |

### Class abilities

- Engineers build sentries and dispensers with FF's commands.
- Medics heal and cure teammates with the medkit.
- Engineers repair teammates' armour.
- Bots pick up backpacks for health, ammo and armour.
- Spies disguise.
- Everyone primes and throws FF grenades. Laser and slowfield grenades are used for area denial.

### How bots understand the map's game mode

FF game modes are pure Lua. The plugin ships a small Lua script, `maps/globalscripts/ffnb_botinfo.lua`,
which FF loads after every map script (`sv_globalluascript`). The script lists the map's Lua
objectives: flags, caps and the items they accept, balls, backpacks, command points and their owners,
Hunted escape zones and team doors. It reports them through FF's `luaevent` game event. It only reads
game state, never changes it. Every step runs under `pcall`, and a map's own `startup()` behaves
exactly as before.

## Requirements

- A Fortress Forever 2013 **Windows** dedicated server. Linux isn't supported yet (see Known gaps).
- Metamod:Source 1.11, 1.12 or 2.0.
- Nav meshes for your maps: the release's nav zip, or generate your own (see below).

## Install

1. Download `fortressforever-nextbot-<version>-windows.zip` from the releases and extract it into the
   server's `fortressforever/` directory. It adds `addons/metamod/ffnextbot.vdf`, `addons/ffnextbot/`,
   `maps/globalscripts/ffnb_botinfo.lua`, `cfg/ffnextbot.example.cfg` and an optional SourceMod
   plugin (see below).
2. Extract `fortressforever-nextbot-navmeshes-<version>.zip` into the same directory, which adds
   `addons/ffnextbot/nav/*.nav`.
3. Copy the settings you want from `cfg/ffnextbot.example.cfg` into `cfg/server.cfg`.
4. Restart the server, and check with `meta list` and `ffnb_status`.

**`sv_globalluascript`:** the plugin sets it to `ffnb_botinfo` when it's empty. If your server
already uses a global script, copy `ffnb_botinfo.lua` to `maps/includes/` and add
`IncludeScript("ffnb_botinfo")` at the end of your global script. The bots still run without the census, but they won't understand objectives.

**Optional: Civilian on every map.** The zip also contains a small SourceMod plugin,
`addons/sourcemod/plugins/ff_civilian_anywhere.smx` (source in [`sourcemod/`](sourcemod/scripting/)).
It's independent of the bots, and just for fun: it lets players and bots pick the Civilian on maps
that don't normally allow it. It does this by changing only the Civilian entry of each active team's
class limits, so map scripts stay stock.

| Cvar | Default | Meaning |
|---|---|---|
| `sm_civanywhere_enabled` | 1 | Master switch |
| `sm_civanywhere_exclude` | `ff_hunted*,hunted*,ff_training` | Maps (wildcards allowed) where the map's own rules are kept |
| `sm_civanywhere_limit` | 0 | Civilian limit per team where it's added (0 = unlimited) |

It needs SourceMod with SDK Tools. Delete the `.smx` if you don't want it.

**Upgrading from another FF bot plugin:** remove or disable its Metamod entry; only one bot plugin
should drive FF's fake clients.

## Configuration

The bot cvars mirror TF2's `tf_bot_*` set under the `ff_bot_` prefix:

- `ff_bot_quota`, `ff_bot_quota_mode` (`normal`, `fill` or `match`), `ff_bot_difficulty`,
  `ff_bot_join_after_player`, `ff_bot_auto_vacate`, `ff_bot_force_class`, `ff_bot_add`,
  `ff_bot_kick` and so on.

FF-specific settings:

| Cvar | Default | Meaning |
|---|---|---|
| `ffnb_prevent_hibernation` | 1 | Keep an empty server simulating. FF otherwise hibernates and kicks bots. |
| `ff_bot_ctf_defense_ratio` | 0.3 | Share of each team that defends its flag in CTF |
| `ff_bot_medic_follow` | 0 | TF2-style medics that follow one patient |
| `ff_bot_grenades` | 1 | Bots use hand grenades |
| `ff_bot_armor_ratio` | 0.5 | Look for armour below this fraction of the class maximum |
| `ffnb_census_script` | 1 | Set `sv_globalluascript ffnb_botinfo` when it's empty |
| `ffnb_spawnroom_radius` | 600 | Nav areas this close to a team spawn count as its spawn room |

### Debug commands

| Command | Shows |
|---|---|
| `ffnb_status` | Every bot's behaviour stack |
| `ffnb_census` | The map's Lua objectives |
| `ffnb_teams` | Teams, allies and class limits |
| `ffnb_navinfo` | Nav mesh decoration |
| `ffnb_path <bot>` | A bot's current path |
| `ffnb_area <id>` | One nav area's details |
| `ffnb_navat x y z [bot]` | The nav area at a point, and whether a bot can reach it |
| `ffnb_perf [reset]` | Frame profile since the last reset: server frame time, and the plugin's own time split into bot movement, bot thinking, path searches and vision. Use it to tell bot load apart from other server stutter. |

## Nav meshes

Bots need a nav mesh per map, kept in `addons/ffnextbot/nav/<map>.nav` (never in `maps/`). To make
one on a **test** server (it kicks bots while it runs):

```
ffnb_nav_generate          // full generation seeded from every team spawn; saved when done
changelevel <map>          // reload to use it
ffnb_nav_seed_ladders go   // optional: add areas at uncovered ladder tops
```

The generator walks through FF's team doors and clips. Ladder volumes come from the BSP's
`info_ladder` records, listed in `addons/ffnextbot/data/ladders.cfg`. `tools/ladders.py` regenerates
that file for your maps.

## Build

You need Python 3, plus Visual Studio 2022 Build Tools with the x86 C++ toolset, and checkouts of:
- [alliedmodders/hl2sdk](https://github.com/alliedmodders/hl2sdk), branch `sdk2013`;
- [alliedmodders/metamod-source](https://github.com/alliedmodders/metamod-source), tag `1.12.0.1228`.

Put them in `deps/hl2sdk` and `deps/metamod-source`, or set the `HL2SDK` / `MMSOURCE` environment
variables. Then:

```
python tools/build.py          ->  build/ffnextbot.dll
python tools/package.py plugin <version>
```

GitHub Actions builds every push and publishes release zips for `v*` tags.

## Known gaps

- **Linux:** FF ships a Linux `server.so`, but the FF game layer hasn't been ported or tested there.
- **Spawn rooms:** they are inferred from team spawn names or teams. Maps whose spawns carry no team
  (some A/D maps) get no spawn-room or incursion data, so bots play somewhat less tactically there.
- **Untested class tricks:** conc-jumping, rocket-jumping and the scout's mancannon aren't used yet.
- **Broken map scripts:** a map script that errors in `startup()` breaks that map's own phase logic,
  and the bots can only follow what the Lua actually does.

## How it works (short)

- **Vendored code:** Valve's NextBot, nav mesh and TFBot behaviours (Source SDK 2013) live in
  `third_party/valve`. Changes for FF are marked `// FFNB:`.
- **Game layer:** `src/shim` provides proxy entities over FF's real entities, reading networked
  fields by name. Players report their feet as their origin (FF's hull is centred); the team
  numbering follows FF.
- **Driving the bots:** they are FF fake clients made with the engine's bot manager. Each frame the
  plugin calls `RunPlayerMove`, and a `PlayerRunCommand` hook lets TFBot write the usercmd.
- **Objectives:** `src/game/ff_census.*` turns the Lua census into flag, cap and zone objects for the
  TFBot scenario code.

## License and credits

- The NextBot, TFBot and nav code is from Valve's Source SDK 2013, under the Source 1 SDK License
  ([LICENSE](LICENSE), [thirdpartylegalnotices.txt](thirdpartylegalnotices.txt)).
- Fortress Forever is by the Fortress Forever team. This is an unofficial community plugin.
- Keeping an empty server awake by forcing the engine's "have any clients" state comes from Bacardi's
  "DelayHibernate" SourceMod plugin. This plugin is an independent C++ reimplementation of that
  technique. Full credit to Bacardi.
