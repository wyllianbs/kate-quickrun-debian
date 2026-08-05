# Changelog

All notable changes to Kate Quick Run will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.0] — 2026-08-05

### Added

- **Initial release** of Kate Quick Run plugin
- **One-shortcut compilation and execution** — `Ctrl+F5` to compile and run
- **Multi-language support:**
  - C / C++
  - Python
  - Java
  - Rust
  - Go
  - JavaScript (Node.js)
  - Shell Script
  - Custom languages (user-configurable)

- **Three run destinations:**
  - Quick Run Terminal (dedicated dock widget)
  - Kate Terminal (embedded, via KParts)
  - External Terminal (Konsole, GNOME Terminal, etc.)

- **Smart focus handling** — focus moves to terminal only when input is needed
- **Configuration panel** — customize per language:
  - Compile flags
  - Run arguments
  - Terminal selection
  - Toolbar icon
  - Button text

- **Security features:**
  - No D-Bus usage for terminal control (uses TerminalInterface API)
  - Shell-injection prevention (POSIX quoting)
  - Safe temporary file handling

- **Internationalization:**
  - English (default)
  - Brazilian Portuguese (pt_BR)

- **KDE Integration:**
  - `.kde-ci.yml` for automated testing
  - AppStream metainfo (`metainfo.xml`)
  - Debian packaging (`debian/`)
  - AUR support (`PKGBUILD`)

### Platform Support

- **Wayland** ✓ (primary)
- **X11** ✓ (expected, not yet verified)
- **KDE Plasma 6** ✓
- **Kate 25.04+** ✓
- **Qt6** ✓
- **KF6** ✓

### Known Limitations

- Window snapping for external terminals uses KWin Scripting (may vary by distro)
- X11 testing pending
- Translations must be submitted via KDE Translation platform

---

## Roadmap

### v1.1 (Planned)

- [ ] Web-based configuration UI improvements
- [ ] Per-project configuration (project-level `.kate-quickrun`)
- [ ] Macro expansion in compile/run commands
- [ ] Output history/logging
- [ ] Keyboard shortcut customization in UI

### v1.2 (Future)

- [ ] Debug mode (GDB integration)
- [ ] Performance profiling
- [ ] Output syntax highlighting
- [ ] Additional language templates

---

## How to Contribute

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines on:
- Reporting bugs
- Suggesting features
- Submitting code changes
- Translations

---

**Author:** Prof. Wyllian Bezerra da Silva — <wyllianbs@gmail.com>
