"""Opt-in real Ticket HTTP/Chromium RAW, correspondence and time journeys."""
import json
import os
from pathlib import Path
import subprocess
import unittest
import sqlite3
from contextlib import closing
import test_ticket_communications as communications
import test_tickets as tickets


@unittest.skipUnless(os.environ.get("PUNTO_TICKET_MAIL_BROWSER_TEST") == "1", "Optional Ticket mail Chromium journeys")
class TicketMailBrowserTests(unittest.TestCase):
    for name in ("request", "api", "login", "create_ticket", "update_ticket", "stop", "mailbox", "authorize",
                 "seed", "workflow", "claim", "convert", "start", "setUp", "connect"):
        locals()[name] = getattr(communications.TicketCommunicationsTests, name)

    @classmethod
    def setUpClass(cls):
        communications.TicketCommunicationsTests.setUpClass.__func__(cls)

    def test_raw_conversion_conversation_time_desktop_mobile(self):
        self.connect()
        raws = [self.seed(job=21) for _ in range(2)]
        config = {"origin": self.origin, "cookie": self.admin["cookie"], "raw_ids": [row["id"] for row in raws]}
        path = Path(self.temporary.name) / "ticket-mail-browser.json"
        path.write_text(json.dumps(config))
        result = subprocess.run([os.environ.get("PUNTO_NODE_BINARY", "node"), str(tickets.ROOT / "tests/ticket_mail_browser.cjs"), str(path)],
                                cwd=tickets.ROOT, env=os.environ.copy(), capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(self.punto.entries), 2)
        print(result.stdout.strip())

    def test_uncertain_time_reload_resumes_existing_operation_desktop_mobile(self):
        self.connect()
        for index, width in enumerate((1280, 375), 1):
            with self.subTest(width=width):
                self.punto.lose_ack = True
                raw = self.seed(job=21)
                config = {"origin": self.origin, "cookie": self.admin["cookie"], "raw_ids": [raw["id"]],
                          "widths": [width], "lost_ack": True}
                path = Path(self.temporary.name) / ("ticket-mail-retry-%s.json" % width)
                path.write_text(json.dumps(config))
                result = subprocess.run([os.environ.get("PUNTO_NODE_BINARY", "node"), str(tickets.ROOT / "tests/ticket_mail_browser.cjs"), str(path)],
                                        cwd=tickets.ROOT, env=os.environ.copy(), capture_output=True, text=True, timeout=90)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(len(self.punto.entries), index, "a reload must never create duplicate remote hours")
                self.assertEqual(len(self.punto.posts), index, "recovery must only look up the original remote request")
                with closing(sqlite3.connect(self.environment["TICKETS_DB_PATH"])) as db:
                    self.assertEqual(db.execute("SELECT count(*) FROM ticket_time_entries").fetchone()[0], index)
                    self.assertEqual(db.execute("SELECT count(*) FROM ticket_time_entries WHERE state='applied'").fetchone()[0], index)
                print(result.stdout.strip())
