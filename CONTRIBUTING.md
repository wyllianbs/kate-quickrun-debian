# Contributing to Kate Quick Run

Thank you for your interest in contributing! This document explains how to report bugs, suggest features, and submit code.

---

## 🐛 Report Bugs

Found a bug? Please open an issue on our [issue tracker](https://invent.kde.org/wyllian/kate-quickrun/-/issues):

1. **Check existing issues** — your bug might already be reported.
2. **Include details:**
   - Kate version (`Help → About Kate`)
   - KDE Plasma version (`About System Settings`)
   - Your Linux distro
   - Steps to reproduce the bug
   - Expected vs. actual behavior
   - Any error messages from the console

---

## 💡 Suggest Features

Have an idea? Open an issue with the label **`enhancement`**:

1. Describe the feature clearly.
2. Explain why it would be useful.
3. Provide examples if possible.

---

## 🔧 Submit Code Changes

### Prerequisites

- **Linux system** with KDE Plasma 6 and Kate 25.04+
- **Build tools:**
  ```bash
  sudo apt install build-essential cmake extra-cmake-modules gettext \
    qt6-base-dev libkf6texteditor-dev libkf6parts-dev libkf6i18n-dev \
    libkf6iconthemes-dev libkf6service-dev libkf6xmlgui-dev \
    libkf6coreaddons-dev libkf6config-dev
  ```

### Steps

1. **Fork the repository** (or clone it directly):
   ```bash
   git clone https://invent.kde.org/wyllian/kate-quickrun.git
   cd kate-quickrun
   ```

2. **Create a feature branch:**
   ```bash
   git checkout -b fix/issue-name
   ```
   or
   ```bash
   git checkout -b feature/new-feature
   ```

3. **Build locally:**
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
   cmake --build build
   ```

4. **Test your changes:**
   ```bash
   # Install to ~/.local (no sudo needed)
   cmake --install build --prefix ~/.local
   kbuildsycoca6 --noincremental
   kate &
   ```

5. **Commit with a clear message:**
   ```bash
   git commit -m "Fix: describe your fix clearly"
   ```

6. **Push to your fork and open a Merge Request** on [invent.kde.org](https://invent.kde.org/wyllian/kate-quickrun/-/merge_requests/new)

---

## 📝 Code Style

- Follow **KDE/Qt6 conventions**.
- Use **camelCase** for variables and functions.
- Use **PascalCase** for classes.
- **Format comments** as `// description` (one-liners) or `/* block */`.
- Avoid trailing whitespace.

---

## 🌍 Translations

Translations are managed through **KDE Translation** (not GitHub/Invent).

To add or update a language:
1. Extract strings:
   ```bash
   xgettext -k -k_ -kN_ kate-quickrun.cpp -o po/kate-quickrun.pot
   ```
2. Create or update the `.po` file in `po/xx_YY/`.
3. Submit via the [KDE Translation platform](https://l10n.kde.org/).

---

## ✅ Pull Request Checklist

Before submitting a merge request, ensure:

- [ ] Code builds without warnings (`-Wall -Wextra`)
- [ ] Tested on Wayland and/or X11
- [ ] Commit messages are clear
- [ ] No breaking API changes (if possible)
- [ ] Related issue is mentioned (`Fixes #123`)

---

## 📞 Questions?

Feel free to open an issue or start a discussion. We're here to help!

---

**Happy coding!** 🚀
