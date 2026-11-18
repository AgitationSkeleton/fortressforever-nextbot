#!/usr/bin/env python3
"""Build the release zips (drop-in layout for the Fortress Forever game directory, fortressforever/).

  python tools/package.py plugin <version> [--binary PATH] [--smx PATH] [--out DIR]
  python tools/package.py navs <version> <nav-dir> [--out DIR]

plugin: addons/metamod/ffnextbot.vdf, addons/ffnextbot/bin/ffnextbot.dll, addons/ffnextbot/data/*,
        maps/globalscripts/ffnb_botinfo.lua, cfg/ffnextbot.example.cfg and the license files
        under addons/ffnextbot/; with --smx, the optional ff_civilian_anywhere SourceMod plugin
        (addons/sourcemod/plugins/) and its source (addons/sourcemod/scripting/).
navs:   addons/ffnextbot/nav/*.nav
"""
import glob, os, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VDF = '"Metamod Plugin"\n{\n\t"alias"\t"ffnextbot"\n\t"file"\t"addons/ffnextbot/bin/ffnextbot"\n}\n'
EXAMPLE_CFG = """// FF NextBot example settings: copy the lines you want into cfg/server.cfg.
// The bot cvars mirror TF2's tf_bot_* set under the ff_bot_ prefix.
ff_bot_quota 12               // number of bots
ff_bot_quota_mode fill        // normal | fill | match
ff_bot_join_after_player 0    // 0 = bots play on an empty server
ff_bot_difficulty 1           // 0 easy, 1 normal, 2 hard, 3 expert
ffnb_prevent_hibernation 1    // keep an empty server simulating (FF would otherwise kick the bots)
ff_bot_ctf_defense_ratio 0.3  // share of each team that defends its flag in CTF
ff_bot_medic_follow 0         // 1 = TF2-style medics that follow one patient
"""


def opt(args, name, default=None):
    return args[args.index(name) + 1] if name in args else default


def add(z, src, arc):
    z.write(src, arc)
    print("  %-60s %9d" % (arc, os.path.getsize(src)))


def plugin(args):
    version = args[0]
    binary = opt(args, "--binary", os.path.join(ROOT, "build", "ffnextbot.dll"))
    out = opt(args, "--out", os.path.join(ROOT, "dist"))
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, "fortressforever-nextbot-%s-windows.zip" % version)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("addons/metamod/ffnextbot.vdf", VDF)
        add(z, binary, "addons/ffnextbot/bin/ffnextbot.dll")
        for f in sorted(glob.glob(os.path.join(ROOT, "package", "addons", "ffnextbot", "data", "*"))):
            add(z, f, "addons/ffnextbot/data/" + os.path.basename(f))
        add(z, os.path.join(ROOT, "package", "fortressforever", "maps", "globalscripts", "ffnb_botinfo.lua"),
            "maps/globalscripts/ffnb_botinfo.lua")
        z.writestr("cfg/ffnextbot.example.cfg", EXAMPLE_CFG)
        smx = opt(args, "--smx")
        if smx:
            add(z, smx, "addons/sourcemod/plugins/ff_civilian_anywhere.smx")
            add(z, os.path.join(ROOT, "sourcemod", "scripting", "ff_civilian_anywhere.sp"),
                "addons/sourcemod/scripting/ff_civilian_anywhere.sp")
        for f in ("LICENSE", "thirdpartylegalnotices.txt", "README.md"):
            add(z, os.path.join(ROOT, f), "addons/ffnextbot/" + f)
    print(path)


def navs(args):
    version, navdir = args[0], args[1]
    out = opt(args, "--out", os.path.join(ROOT, "dist"))
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, "fortressforever-nextbot-navmeshes-%s.zip" % version)
    files = sorted(glob.glob(os.path.join(navdir, "*.nav")))
    if not files:
        sys.exit("no .nav files in " + navdir)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            add(z, f, "addons/ffnextbot/nav/" + os.path.basename(f))
    print(path, len(files), "meshes")


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] not in ("plugin", "navs"):
        sys.exit(__doc__)
    (plugin if sys.argv[1] == "plugin" else navs)(sys.argv[2:])
