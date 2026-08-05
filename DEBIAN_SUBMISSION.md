# Kate Quick Run — Debian Official Submission

This repository contains the official Debian package for Kate Quick Run plugin.

## Ready for Submission

### Prerequisites Met
✅ Source code complete and tested  
✅ Debian packaging (debian/) configured  
✅ 9 language translations (po/)  
✅ Documentation complete  
✅ GPL-2.0-or-later license  
✅ CMake build system  
✅ KDE/Qt6 dependencies declared  

### Files Structure

```
.
├── debian/              ← Debian packaging configuration
│   ├── changelog        ← Version history
│   ├── control          ← Metadata & dependencies
│   ├── copyright        ← License information
│   ├── rules            ← Build instructions
│   ├── source/
│   │   └── format       ← Source format (3.0 quilt)
│   └── watch            ← Upstream release tracking
├── po/                  ← Translations (9 languages)
├── docs/                ← Documentation & screenshots
├── icons/               ← Application icons
├── CMakeLists.txt       ← Build configuration
├── kate-quickrun.*      ← Source code
├── README.md            ← User documentation
├── CONTRIBUTING.md      ← Contributor guide
├── CHANGELOG.md         ← Version history
├── PACKAGING.md         ← Maintainer guide
└── LICENSE              ← GPL-2.0-or-later
```

## Debian Submission Steps

Process per the official guide: https://mentors.debian.net/intro-maintainers/

### 1. Local Build & Test
```bash
cd QuickRun-Debian
dpkg-buildpackage -b -us -uc -tc      # Build binary package
dpkg-buildpackage -S -sa              # Build source package (needs a GPG-signed .dsc)
lintian ../kate-quickrun_*.changes    # Must be free of errors before upload
```

### 2. File the ITP (Intent To Package)
The ITP is a bug against the `wnpp` pseudo-package, not a mailing-list post.
Easiest via `reportbug`:
```bash
reportbug --severity=wishlist --package=wnpp
```
- Subject: `ITP: kate-quickrun -- compile and run plugin for Kate`
- Body: short description, upstream URL, license, why it's useful
- This opens a bug on `wnpp@bugs.debian.org` and is automatically mirrored
  to `debian-devel@lists.debian.org`; no separate email is needed.
- Note the bug number (e.g. `#123456`) — reference it as `Closes: #123456`
  in `debian/changelog` once the package is uploaded.

### 3. Create a mentors.debian.net account
- Register at https://mentors.debian.net/
- Add a GPG key to the account (used to sign the upload)
- Configure `~/.dput.cf` for the mentors host (see site instructions)

### 4. Sign and upload the source package
```bash
debsign ../kate-quickrun_1.0.1-1_source.changes
dput mentors ../kate-quickrun_1.0.1-1_source.changes
```
- On the package's mentors.debian.net page, enable "Needs a Sponsor"

### 5. Request sponsorship (RFS)
- File an RFS bug against the `sponsorship-requests` pseudo-package
  (via `reportbug sponsorship-requests`), linking the mentors.debian.net
  package page and the ITP bug number

### 6. Review & Approval
- A Debian Developer sponsor reviews for policy compliance and quality
- Address feedback, re-upload to mentors, and re-notify the sponsor as needed
- New packages additionally require ftpmaster approval (including a
  copyright-file check) before entering the archive
- Once accepted, the ITP bug is closed automatically by the upload

### 7. Cascading to Other Distros
- Ubuntu inherits automatically
- Linux Mint, Elementary OS, Pop!_OS, etc. follow
- ~30 Debian-based distributions benefit

## Package Information

| Field | Value |
|-------|-------|
| **Name** | kate-quickrun |
| **Version** | 1.0.1-1 |
| **License** | GPL-2.0-or-later |
| **Section** | editors |
| **Priority** | optional |
| **Maintainer** | Prof. Wyllian Bezerra da Silva <wyllianbs@gmail.com> |

## Dependencies

**Build:** cmake, extra-cmake-modules, gettext, qt6-base-dev, libkf6*  
**Runtime:** kate, konsole-kpart (optional)

## Translations

- English (default)
- Português Brasileiro (pt_BR)
- Español (es)
- Français (fr)
- Deutsch (de)
- Italiano (it)
- Русский (ru)
- 中文简体 (zh_CN)
- 日本語 (ja)

## Status

**Ready for:** ITP submission → Mentors upload → Debian review → Official package

**Awaiting:** KDE approval (parallel process)

---

*Last updated: 2026-08-05*
*Maintainer: Prof. Wyllian Bezerra da Silva <wyllianbs@gmail.com>*
