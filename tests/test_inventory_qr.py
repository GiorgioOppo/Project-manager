"""Local QR labels: safe DOM, usable links, print cleanup, and native decoding."""
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[1]
NODE = (os.environ.get("NODE") or shutil.which("node") or
        str(Path.home() / ".cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node"))
HARNESS = r"""
const fs = require('fs'), vm = require('vm');
const request = JSON.parse(fs.readFileSync(0, 'utf8'));
const listeners = new Map(), timers = new Map(), classes = new Set();
let timerId = 0, savedBlob = null, clicked = null, printed = null, revoked = false;
class Element {
  constructor(name) { this.name = name; this.attributes = {}; this.children = []; this.text = ''; }
  setAttribute(key, value) { this.attributes[key] = value; }
  set textContent(value) { this.text = value; }
  set innerHTML(value) { throw new Error('Unsafe HTML insertion'); }
  append(...children) { children.forEach(child => {child.parent = this; this.children.push(child);}); }
  remove() { if (this.parent) this.parent.children = this.parent.children.filter(child => child !== this); }
  click() { clicked = {href: this.href, download: this.download}; }
}
const body = new Element('body');
body.classList = {add: value => classes.add(value), remove: value => classes.delete(value)};
const escape = value => String(value).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
const tree = node => ({name: node.name, attributes: node.attributes, text: node.text, children: node.children.map(tree)});
const serialize = node => `<${node.name}${Object.entries(node.attributes).map(([key,value]) => ` ${key}="${escape(value)}"`).join('')}>${escape(node.text)}${node.children.map(serialize).join('')}</${node.name}>`;
class TestURL extends URL {
  static createObjectURL(blob) {savedBlob = blob; return 'blob:https://punto.example/test';}
  static revokeObjectURL(url) {revoked = true;}
}
const context = {
  document: {body, createElement: name => new Element(name), createElementNS: (namespace, name) => new Element(name)},
  location: {origin: 'https://punto.example:8443'}, URL: TestURL, TextEncoder, Blob,
  XMLSerializer: class {serializeToString(value) {return serialize(value);}},
  setTimeout: fn => {timers.set(++timerId, fn); return timerId;},
  clearTimeout: id => timers.delete(id),
  addEventListener: (name, fn) => listeners.set(name, fn),
  removeEventListener: (name, fn) => {if (listeners.get(name) === fn) listeners.delete(name);},
  print: () => {
    printed = {classes: Array.from(classes), children: body.children.map(tree)};
    if (request.failPrint) throw new Error('Print unavailable');
    if (request.afterPrint && listeners.has('afterprint')) listeners.get('afterprint')();
  }
};
context.window = context;
vm.createContext(context);
vm.runInContext(fs.readFileSync(process.argv[1] + '/public/qrcodegen.js', 'utf8'), context);
vm.runInContext(fs.readFileSync(process.argv[1] + '/public/inventory-qr.js', 'utf8'), context);
(async () => {
  try {
    let result;
    if (request.op === 'url') result = {url: context.InventoryQr.assetUrl(request.id, request.origin)};
    else if (request.op === 'download') {
      context.InventoryQr.downloadLabel(request.asset);
      result = {clicked, svg: await savedBlob.text(), children: body.children.length};
      Array.from(timers.values()).forEach(fn => fn()); result.revoked = revoked;
    } else if (request.op === 'print') {
      context.InventoryQr.printLabel(request.asset);
      if (request.cleanupFallback) Array.from(timers.values()).forEach(fn => fn());
      result = {printed, classes: Array.from(classes), children: body.children.length, listeners: listeners.size};
    } else result = {svg: tree(context.InventoryQr.createLabel(request.asset))};
    console.log(JSON.stringify({ok: true, ...result}));
  } catch (error) {console.log(JSON.stringify({ok: false, error: error.message, classes: Array.from(classes), children: body.children.length}));}
})();
"""


@unittest.skipUnless(Path(NODE).is_file() or shutil.which(NODE), "Node.js not available")
class InventoryQrTest(unittest.TestCase):
    asset = {"id": 42, "asset_tag": "IT-0042", "name": "Portatile amministrazione"}

    def run_helper(self, **request):
        result = subprocess.run([NODE, "-e", HARNESS, str(ROOT)],
                                input=json.dumps(request), capture_output=True,
                                text=True, check=True, timeout=15)
        return json.loads(result.stdout)

    def test_absolute_deep_link_and_valid_identifier(self):
        result = self.run_helper(op="url", id=42)
        self.assertEqual(result["url"], "https://punto.example:8443/app#inventory?asset_id=42")
        self.assertEqual(self.run_helper(op="url", id="42", origin="https://assets.example/path")["url"],
                         "https://assets.example/app#inventory?asset_id=42")
        for value in (0, -1, 1.1, "01", "42&role=manager", 9007199254740992, True):
            with self.subTest(id=value):
                self.assertFalse(self.run_helper(op="url", id=value)["ok"])

    def test_only_http_origins_without_credentials(self):
        for origin in ("javascript:alert(1)", "data:text/html,test", "https://user:password@example.com"):
            with self.subTest(origin=origin):
                self.assertFalse(self.run_helper(op="url", id=42, origin=origin)["ok"])

    def test_svg_has_accessible_description_and_quiet_zone(self):
        svg = self.run_helper(asset=self.asset)["svg"]
        self.assertEqual(svg["attributes"]["width"], "90mm")
        self.assertEqual(svg["attributes"]["height"], "50mm")
        self.assertEqual(svg["attributes"]["role"], "img")
        self.assertIn("IT-0042", svg["children"][0]["text"])
        self.assertIn("Accesso all’app richiesto", svg["children"][1]["text"])
        group = next(node for node in svg["children"] if node["name"] == "g")
        coordinates = [(int(x), int(y)) for x, y in re.findall(r"M(\d+),(\d+)h1v1h-1z", group["children"][0]["attributes"]["d"])]
        self.assertTrue(coordinates)
        self.assertEqual(min(x for x, _ in coordinates), 4)
        self.assertEqual(min(y for _, y in coordinates), 4)

    def test_asset_text_is_never_inserted_as_markup(self):
        asset = {"id": 42, "asset_tag": '<script>"&', "name": '<img src=x onerror="alert(1)">' }
        result = self.run_helper(op="download", asset=asset)
        self.assertTrue(result["ok"])
        self.assertIn("&lt;script&gt;&quot;&amp;", result["svg"])
        self.assertNotIn("<script>", result["svg"])
        self.assertNotIn("<img ", result["svg"])
        self.assertNotIn("<foreignObject", result["svg"])
        self.assertTrue(result["clicked"]["download"].endswith(".svg"))
        self.assertNotIn("/", result["clicked"]["download"])
        self.assertEqual(result["children"], 0)
        self.assertTrue(result["revoked"])

    def test_long_labels_fit_the_printable_area(self):
        for asset in ({"id": 42, "asset_tag": "T" * 64, "name": ("Parolalunghissima " * 10)[:160]},
                      {"id": 42, "asset_tag": "漢" * 21, "name": "漢" * 53}):
            with self.subTest(asset=asset):
                result = self.run_helper(asset=asset)
                self.assertTrue(result["ok"])
                texts = [node for node in result["svg"]["children"] if node["name"] == "text"]
                self.assertTrue(all(float(node["attributes"]["y"]) <= 195 for node in texts))
                self.assertTrue(all(float(node["attributes"]["font-size"]) >= 8 for node in texts))
                for node in texts:
                    if node["attributes"].get("font-family") == "monospace":
                        cells = sum(2 if char == "漢" else 1 for char in node["text"])
                        self.assertLessEqual(cells * float(node["attributes"]["font-size"]) * 0.6, 176)

    def test_invalid_asset_text_is_rejected(self):
        for update in ({"asset_tag": ""}, {"name": ""}, {"asset_tag": "à" * 33}, {"name": "à" * 81}):
            with self.subTest(update=update):
                self.assertFalse(self.run_helper(asset={**self.asset, **update})["ok"])

    def test_print_includes_only_label_and_cleans_up(self):
        result = self.run_helper(op="print", asset=self.asset, afterPrint=True)
        self.assertTrue(result["ok"])
        self.assertEqual(result["printed"]["classes"], ["inventory-qr-printing"])
        self.assertEqual(result["printed"]["children"][0]["children"][0]["name"], "svg")
        self.assertEqual(result["classes"], [])
        self.assertEqual(result["children"], 0)
        self.assertEqual(result["listeners"], 0)

    def test_print_error_and_cancellation_fallback_clean_up(self):
        failed = self.run_helper(op="print", asset=self.asset, failPrint=True)
        self.assertFalse(failed["ok"])
        self.assertEqual(failed["classes"], [])
        self.assertEqual(failed["children"], 0)
        cancelled = self.run_helper(op="print", asset=self.asset, cleanupFallback=True)
        self.assertTrue(cancelled["ok"])
        self.assertEqual(cancelled["classes"], [])
        self.assertEqual(cancelled["children"], 0)

    @unittest.skipUnless(sys.platform == "darwin" and shutil.which("swift"), "macOS Vision QR decoder not available")
    def test_generated_qr_decodes_to_the_exact_asset_link(self):
        """Decode actual output modules with Apple's independent Vision scanner."""
        svg = self.run_helper(asset=self.asset)["svg"]
        group = next(node for node in svg["children"] if node["name"] == "g")
        scale = float(re.search(r"scale\(([^)]+)\)", group["attributes"]["transform"])[1])
        extent = round(162 / scale)
        modules = {(int(x), int(y)) for x, y in re.findall(r"M(\d+),(\d+)h1v1h-1z", group["children"][0]["attributes"]["d"])}
        pixels = extent * 8
        image = b"".join(b"\0" + bytes(0 if (x // 8, y // 8) in modules else 255 for x in range(pixels))
                         for y in range(pixels))
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", pixels, pixels, 8, 0, 0, 0, 0)) +
               chunk(b"IDAT", zlib.compress(image)) + chunk(b"IEND", b""))
        source = '''import Foundation
import Vision
let request = VNDetectBarcodesRequest()
request.revision = VNDetectBarcodesRequestRevision1
request.usesCPUOnly = true
request.symbologies = [.qr]
try VNImageRequestHandler(url: URL(fileURLWithPath: CommandLine.arguments[1]), options: [:]).perform([request])
let values = (request.results ?? []).compactMap { $0.payloadStringValue }
let data = try JSONSerialization.data(withJSONObject: values)
print(String(data: data, encoding: .utf8)!)
'''
        with tempfile.TemporaryDirectory(prefix="punto-qr-test-") as directory:
            path = Path(directory)
            (path / "qr.png").write_bytes(png)
            (path / "decode.swift").write_text(source)
            result = subprocess.run(["swift", "-module-cache-path", str(path / "cache"),
                                     str(path / "decode.swift"), str(path / "qr.png")],
                                    capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout), ["https://punto.example:8443/app#inventory?asset_id=42"])


if __name__ == "__main__":
    unittest.main()
