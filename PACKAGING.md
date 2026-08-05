# Packaging & maintainer notes

Notes for building, translating and packaging Kate Quick Run. End-user
install instructions are in [README.md](README.md).

## Build from source

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build
kbuildsycoca6 --noincremental
```

## Translations

Message catalogs live in `po/<lang>/kate-quickrun.po`, generated from the
template `po/kate-quickrun.pot`. `ki18n_install(po)` (in
[CMakeLists.txt](CMakeLists.txt)) compiles and installs them as `.mo`
catalogs at build time — no manual step is required beyond editing the
`.po` files.

To add a new language:

1. Copy `po/kate-quickrun.pot` to `po/<lang>/kate-quickrun.po`.
2. Translate the `msgstr` entries.
3. Rebuild; the new catalog is picked up automatically.

To refresh an existing `.po` against the current `.pot` (after source
strings change), use `msgmerge` from `gettext`.

## Debian packaging

The `debian/` directory contains the packaging used for both local builds
and the official Debian submission. See [DEBIAN_SUBMISSION.md](DEBIAN_SUBMISSION.md)
for the full submission process.

Local build/check:

```bash
dpkg-buildpackage -b -us -uc -tc      # binary package
dpkg-buildpackage -S -sa              # source package
lintian ../kate-quickrun_*.changes    # policy/QA check
```

## Arch Linux packaging

`PKGBUILD` (AUR) tracks the same upstream source and mirrors the CMake
build above; see the AUR page for install/update instructions.
