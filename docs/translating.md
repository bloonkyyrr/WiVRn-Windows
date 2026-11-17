# Translating

The headset client is translated through the `.po` files in `locale/<lang>/wivrn.po`.
Each language directory must be listed in the Android build (see `locale/`).

## Checking the catalogs

`tools/check_po.py` verifies that the `.po` files are complete and consistent with the
source; CI runs it on every pull request (see `.github/workflows/Locales.yml`).

```bash
pip install polib
python tools/check_po.py
```

## Editing

Any gettext editor works, for example:

- [Poedit](https://poedit.net/) on Windows
- [Lokalize](https://apps.kde.org/lokalize/)

Wrap lines at the same points as the existing entries and keep the `msgctxt` markers
intact, then run `tools/check_po.py` before opening a pull request.
