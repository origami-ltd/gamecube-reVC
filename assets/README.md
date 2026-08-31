# assets/

Place your Grand Theft Auto: Vice City game data in this folder. Everything
here except this file is ignored by git — no game data is ever committed or
distributed with this repository.

Expected layout:

```
assets/
├── GTAVC/          # your Vice City installation (anim, audio, data,
│                   # models, movies, txd, ...)
└── gamefiles/      # port-specific game data used by the build scripts
    ├── TEXT/       # localisation (.gxt)
    └── neo/        # neo pipeline data
```

With the data in place, generate the SD card tree:

```bash
python3 tools/gamecube/build_sd.py --game assets/GTAVC --out <sd-tree> \
    --txdconv <path-to-txdconv>
```

See `python3 tools/gamecube/build_sd.py --help` for all arguments.
