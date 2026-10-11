#!/usr/bin/env python3
"""Validate the five translation catalogs and copy shared assets into Ticket.

Punto and Ticket have independent public directories and Docker images. Run this
script after editing public/i18n.js, public/i18n.css or public/locales/*.json.
Use --check in CI to detect stale copies without modifying files.
"""
import argparse
import json
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LANGUAGES = ("en", "zh", "es", "fr", "de")


def validate():
    catalogs = {}
    for language in LANGUAGES:
        path = ROOT / "public" / "locales" / f"{language}.json"
        catalog = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(catalog, dict) or not catalog:
            raise ValueError(f"Invalid catalog: {path}")
        for source, translated in catalog.items():
            if not isinstance(translated, str) or not translated.strip():
                raise ValueError(f"Empty translation in {language}: {source}")
            if sorted(re.findall(r"\{\d+\}", source)) != sorted(re.findall(r"\{\d+\}", translated)):
                raise ValueError(f"Invalid placeholders in {language}: {source}")
            for pattern in (r"^\s*", r"\s*$"):
                if re.search(pattern, source).group() != re.search(pattern, translated).group():
                    raise ValueError(f"Invalid surrounding whitespace in {language}: {source!r}")
        catalogs[language] = catalog
    keys = set(catalogs[LANGUAGES[0]])
    if any(set(catalog) != keys for catalog in catalogs.values()):
        raise ValueError("All languages must cover the same messages")
    return len(keys)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    count = validate()
    files = ["i18n.js", "i18n.css", *(f"locales/{language}.json" for language in LANGUAGES)]
    for name in files:
        source = ROOT / "public" / name
        destination = ROOT / "ticket-app" / "public" / name
        if args.check:
            if not destination.is_file() or source.read_bytes() != destination.read_bytes():
                raise ValueError(f"Ticket copy is stale: {destination}")
        else:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
    print(f"{count} messages × {len(LANGUAGES)} languages; Ticket assets {'verified' if args.check else 'synchronized'}.")


if __name__ == "__main__":
    main()
