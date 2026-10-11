"""Catalog coverage and isolated browser checks for Punto and Ticket localization.

Browser checks require PUNTO_I18N_BROWSER_TEST=1, PUNTO_NODE_BINARY and
PUNTO_PLAYWRIGHT_MODULE (or a globally resolvable playwright package).
"""
import importlib.util
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time
import unittest
from html.parser import HTMLParser

ROOT = Path(__file__).resolve().parents[1]
LANGUAGES = ("en", "zh", "es", "fr", "de")


class MarkedText(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack = []
        self.messages = set()

    def handle_starttag(self, tag, attributes):
        attributes = dict(attributes)
        for name in attributes.get("data-i18n-attrs", "").split():
            self.messages.add(attributes[name])
        if tag not in ("input", "br", "hr", "meta", "link", "img", "source"):
            self.stack.append((tag, attributes))

    def handle_startendtag(self, tag, attributes):
        self.handle_starttag(tag, attributes)
        if self.stack and self.stack[-1][0] == tag:
            self.stack.pop()

    def handle_endtag(self, tag):
        while self.stack:
            if self.stack.pop()[0] == tag:
                break

    def handle_data(self, text):
        if self.stack and "data-i18n" in self.stack[-1][1] and text.strip():
            self.messages.add(text.strip())


class LocalizationCatalogTests(unittest.TestCase):
    def test_catalog_integrity_and_ticket_copies(self):
        spec = importlib.util.spec_from_file_location("sync_locales", ROOT / "scripts/sync_locales.py")
        sync = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(sync)
        self.assertGreater(sync.validate(), 1000)
        for name in ["i18n.js", "i18n.css", *(f"locales/{key}.json" for key in LANGUAGES)]:
            self.assertEqual((ROOT / "public" / name).read_bytes(), (ROOT / "ticket-app/public" / name).read_bytes(), name)

    def test_all_marked_static_and_literal_dynamic_messages_are_translated(self):
        catalog = json.loads((ROOT / "public/locales/en.json").read_text(encoding="utf-8"))
        invariant = {"Ticket", "demo@punto.local", "Punto2026!"}
        for name in ("public/index.html", "public/app.html", "ticket-app/public/index.html"):
            parser = MarkedText()
            parser.feed((ROOT / name).read_text(encoding="utf-8"))
            self.assertFalse(parser.messages - set(catalog) - invariant, f"Untranslated HTML in {name}: {parser.messages - set(catalog) - invariant}")
        for name in ("public/app.js", "public/login.js", "ticket-app/public/app.js", "public/inventory-qr.js"):
            messages = {json.loads(match[1]) for match in re.finditer(r'\bt\(("(?:[^"\\]|\\.)*")', (ROOT / name).read_text(encoding="utf-8"))}
            self.assertFalse(messages - set(catalog), f"Untranslated JavaScript in {name}: {messages - set(catalog)}")

    def test_script_order_and_locale_independent_api_codes(self):
        for name in ("public/index.html", "public/app.html", "ticket-app/public/index.html"):
            source = (ROOT / name).read_text(encoding="utf-8")
            self.assertEqual(source.count('id="language-select"'), 1)
            app_script = "/login.js" if name == "public/index.html" else "/app.js"
            self.assertLess(source.index('/i18n.js'), source.index(app_script))
        source = (ROOT / "public/app.js").read_text(encoding="utf-8")
        self.assertNotIn('chooseReportView(t(', source)
        self.assertNotIn('state.reportView === t(', source)
        self.assertIn('currency: "EUR"', source)
        self.assertNotIn('Intl.NumberFormat("it-IT"', source)


@unittest.skipUnless(os.environ.get("PUNTO_I18N_BROWSER_TEST") == "1", "Optional isolated browser test")
class LocalizationBrowserTests(unittest.TestCase):
    def test_six_languages_in_both_apps(self):
        node = os.environ.get("PUNTO_NODE_BINARY", "node")
        with tempfile.TemporaryDirectory(prefix="punto-i18n-") as temporary:
            processes = []
            logs = []
            origins = []
            try:
                for ticket in (False, True):
                    with socket.socket() as reserved:
                        reserved.bind(("127.0.0.1", 0))
                        port = reserved.getsockname()[1]
                    origin = f"http://127.0.0.1:{port}"
                    origins.append(origin)
                    env = dict(os.environ, AUTH_MODE="local", INVENTORY_ADDON_URL="", TICKETS_APP_ORIGIN="", PUNTO_API_ORIGIN="")
                    if ticket:
                        env.update(TICKETS_PORT=str(port), TICKETS_BIND="127.0.0.1", TICKETS_PUBLIC_ORIGIN=origin,
                                   TICKETS_DB_PATH=f"{temporary}/tickets.db", TICKETS_PUBLIC_DIR=str(ROOT / "ticket-app/public"),
                                   TICKETS_EMAIL="i18n@example.test", TICKETS_PASSWORD="I18n-test-only!2026")
                        binary = ROOT / "ticket-app/build/punto-tickets"
                    else:
                        env.update(PORT=str(port), BIND_HOST="127.0.0.1", PUBLIC_PORT=str(port), PUBLIC_ORIGINS=origin,
                                   DB_PATH=f"{temporary}/punto.db", PUBLIC_DIR=str(ROOT / "public"),
                                   PUNTO_EMAIL="i18n@example.test", PUNTO_PASSWORD="I18n-test-only!2026")
                        binary = ROOT / "build/punto"
                    log = tempfile.TemporaryFile()
                    logs.append(log)
                    process = subprocess.Popen([str(binary)], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
                    processes.append(process)
                    for _ in range(200):
                        if process.poll() is not None:
                            log.seek(0)
                            self.fail(log.read().decode("utf-8", "replace"))
                        try:
                            with socket.create_connection(("127.0.0.1", port), timeout=.1):
                                break
                        except OSError:
                            time.sleep(.025)
                    else:
                        self.fail("Isolated test server did not start")
                result = subprocess.run([node, str(ROOT / "tests/i18n_browser_check.cjs"), *origins], cwd=ROOT, env=os.environ.copy(), capture_output=True, text=True, timeout=180)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                print(result.stdout.strip())
            finally:
                for process in processes:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                for log in logs:
                    log.close()


if __name__ == "__main__":
    unittest.main()
