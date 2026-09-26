# assets/

Place your Grand Theft Auto: Vice City game data in this folder. Everything
here except this file is ignored by git — no game data is ever committed or
distributed with this repository.

Expected layout:

```
assets/
├── GTAVC/          # your Vice City installation (anim, audio, data,
│                   # models, movies, txd, ...)
├── gamefiles/      # reVC's gamefiles: TEXT/ (.gxt), neo/, models/, data/
└── movies/         # optional: pre-encoded opening.ogv + titles.ogv
```

With the data in place, build the releases from the repository root:

```bash
python3 build.py iso        # GameCube -> build/release/reVC-GameCube.iso
python3 build.py sd         # Wii      -> build/release/reVC-Wii-SD/ (work in progress)
python3 build.py release    # both
```

or with Docker: `docker run --rm -v "$PWD/assets":/assets:ro -v "$PWD/out":/out revc`.
