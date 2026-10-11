/* NPM 2.16.0 rendered-config filter. Never synthesize a permissive proxy:
 * the callback inherits the existing root location, byte for byte, including
 * its access list, basic authentication, headers and HSTS directives. */
import fs from "node:fs";
import { randomUUID } from "node:crypto";
import path from "node:path";
import { fileURLToPath } from "node:url";

const CALLBACK = "/api/calendar/oauth/callback";
const MARKER = "# punto-oauth-callback-private-v1";
const FAILURE = "OAuth callback privacy: unsupported app proxy configuration; review its locations before starting Nginx.";

function tokens(source) {
  const result = [];
  for (let index = 0; index < source.length;) {
    if (/\s/.test(source[index])) { index++; continue; }
    if (source[index] === "#") {
      while (index < source.length && source[index] !== "\n") index++;
      continue;
    }
    const start = index;
    if ("{};".includes(source[index])) {
      result.push({ value: source[index++], start, end: index, structural: true }); continue;
    }
    let value = "", quote = null;
    if (source[index] === '"' || source[index] === "'") quote = source[index++];
    let ended = !quote;
    while (index < source.length) {
      const character = source[index];
      if (quote && character === quote) { index++; ended = true; break; }
      if (!quote && (/\s/.test(character) || "{};#".includes(character))) break;
      if (character === "\\") {
        if (++index >= source.length) throw new Error(FAILURE);
        value += source[index++];
      } else value += source[index++];
    }
    if (!ended || index === start) throw new Error(FAILURE);
    result.push({ value, start, end: index });
  }
  return result;
}

function parse(source) {
  const words = tokens(source);
  let index = 0;
  function level(nested) {
    const nodes = [];
    while (index < words.length) {
      if (words[index].structural && words[index].value === "}") {
        if (!nested) throw new Error(FAILURE);
        return { nodes, close: words[index++] };
      }
      const header = [];
      const start = words[index].start;
      while (index < words.length && !words[index].structural) header.push(words[index++].value);
      if (!header.length || index >= words.length || words[index].value === "}") throw new Error(FAILURE);
      const delimiter = words[index++];
      if (delimiter.value === ";") nodes.push({ header, start, end: delimiter.end, children: null });
      else {
        const contents = level(true);
        if (!contents.close) throw new Error(FAILURE);
        nodes.push({ header, start, end: contents.close.end, children: contents.nodes,
          bodyStart: delimiter.end, bodyEnd: contents.close.start });
      }
    }
    if (nested) throw new Error(FAILURE);
    return { nodes };
  }
  return level(false).nodes;
}

function locationMatches(node) {
  if (node.header[0] !== "location") return false;
  const [, modifier, address] = node.header;
  if (modifier === "~" || modifier === "~*") return true; // PCRE cannot be safely approximated by JS.
  if (modifier === "=") return address === CALLBACK;
  const prefix = modifier === "^~" ? address : modifier;
  return typeof prefix !== "string" || CALLBACK.startsWith(prefix);
}

function inspectIncludes(nodes, directory) {
  const standard = new Set([
    "conf.d/include/assets.conf", "conf.d/include/block-exploits.conf",
    "conf.d/include/letsencrypt-acme-challenge.conf", "conf.d/include/ssl-cache.conf",
    "conf.d/include/ssl-ciphers.conf", "conf.d/include/force-ssl.conf"
  ]);
  function customSafety(items) {
    for (const item of items) {
      if (locationMatches(item) || ["include", "rewrite", "try_files", "error_page"].includes(item.header[0]))
        throw new Error(FAILURE);
      if (item.children) customSafety(item.children);
    }
  }
  for (const node of nodes) {
    if (["rewrite", "try_files", "error_page"].includes(node.header[0])) throw new Error(FAILURE);
    if (node.header[0] !== "include") continue;
    if (node.header[1] === "/data/nginx/custom/server_proxy[.]conf") {
      const custom = path.join(directory, "server_proxy.conf");
      if (!fs.existsSync(custom)) continue;
      const status = fs.lstatSync(custom);
      if (!status.isFile() || status.isSymbolicLink() || status.size > 1024 * 1024) throw new Error(FAILURE);
      customSafety(parse(fs.readFileSync(custom, "utf8")));
    } else if (!standard.has(node.header[1])) throw new Error(FAILURE);
  }
}

function callbackBody(source, root) {
  const removals = [];
  function inspect(nodes, nested) {
    for (const node of nodes) {
      const directive = node.header[0];
      if (["location", "rewrite", "try_files", "error_page"].includes(directive)) throw new Error(FAILURE);
      if (directive === "include" && node.header[1] !== "conf.d/include/proxy.conf") throw new Error(FAILURE);
      if (directive === "access_log" || directive === "error_log") {
        if (nested) throw new Error(FAILURE);
        removals.push(node);
      }
      if (node.children) inspect(node.children, true);
    }
  }
  inspect(root.children, false);
  let body = source.slice(root.bodyStart, root.bodyEnd);
  for (const node of removals.sort((a, b) => b.start - a.start)) {
    body = body.slice(0, node.start - root.bodyStart) + body.slice(node.end - root.bodyStart);
  }
  return "\n    access_log off;\n    error_log /dev/null crit;\n" + body;
}

export function protectProxyConfig(source, options = {}) {
  if (typeof source !== "string" || source.length > 1024 * 1024) throw new Error(FAILURE);
  const lexical = tokens(source);
  const setTo = (variable, value) => lexical.some((word, index) => word.value === "set" &&
    lexical[index + 1]?.value === variable && lexical[index + 2]?.value === value &&
    lexical[index + 3]?.structural && lexical[index + 3]?.value === ";");
  if (!setTo("$server", "app") || !setTo("$port", "8080")) return source;
  const nodes = parse(source);
  const appServers = nodes.filter(node => node.header.length === 1 && node.header[0] === "server" &&
    node.children.some(item => item.header.join(" ") === "set $server app") &&
    node.children.some(item => item.header.join(" ") === "set $port 8080"));
  if (!appServers.length) return source;
  if (appServers.length !== 1 || nodes.filter(node => node.header[0] === "server").length !== 1) throw new Error(FAILURE);
  const server = appServers[0];
  const roots = server.children.filter(node => node.header.join(" ") === "location /");
  if (roots.length !== 1) throw new Error(FAILURE);
  const root = roots[0];
  const expected = callbackBody(source, root);
  const callbacks = server.children.filter(node => node.header.join(" ") === "location = " + CALLBACK);
  if (source.includes(MARKER)) {
    if (source.split(MARKER).length !== 2 || callbacks.length !== 1 ||
        source.slice(callbacks[0].bodyStart, callbacks[0].bodyEnd) !== expected) throw new Error(FAILURE);
  } else if (callbacks.length) throw new Error(FAILURE);
  for (const node of server.children) {
    if (node !== root && node !== callbacks[0] && locationMatches(node)) throw new Error(FAILURE);
  }
  inspectIncludes(server.children, options.customDirectory || "/data/nginx/custom");
  if (callbacks.length) return source;
  const callback = `\n  ${MARKER}\n  location = ${CALLBACK} {${expected}}\n`;
  return source.slice(0, server.bodyEnd) + callback + source.slice(server.bodyEnd);
}

export function migrateProxyDirectory(directory = "/data/nginx/proxy_host", options = {}) {
  if (!fs.existsSync(directory)) return 0;
  const changes = [];
  for (const name of fs.readdirSync(directory).sort()) {
    if (!/^\d+\.conf$/.test(name)) continue;
    const filename = path.join(directory, name);
    const status = fs.lstatSync(filename);
    if (!status.isFile() || status.isSymbolicLink() || status.size > 1024 * 1024) throw new Error(FAILURE);
    const before = fs.readFileSync(filename, "utf8");
    const after = protectProxyConfig(before, options);
    if (after !== before) changes.push({ filename, status, before, after });
  }
  /* Validate every host before writing any file. Backups deliberately have no
     .conf extension and are readable only by their original owner. */
  for (const change of changes) {
    const backup = change.filename + ".before-oauth-privacy";
    if (!fs.existsSync(backup)) {
      fs.writeFileSync(backup, change.before, { flag: "wx", mode: 0o600 });
      fs.chownSync(backup, change.status.uid, change.status.gid);
    }
    const temporary = change.filename + ".privacy-" + randomUUID();
    try {
      fs.writeFileSync(temporary, change.after, { flag: "wx", mode: change.status.mode & 0o777 });
      fs.chmodSync(temporary, change.status.mode & 0o777);
      fs.chownSync(temporary, change.status.uid, change.status.gid);
      fs.renameSync(temporary, change.filename);
    } finally {
      if (fs.existsSync(temporary)) fs.unlinkSync(temporary);
    }
  }
  return changes.length;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const count = migrateProxyDirectory(process.argv[2], {
      customDirectory: process.argv[3] || "/data/nginx/custom"
    });
    console.log(`OAuth callback log privacy: ${count} app proxy configuration(s) updated.`);
  } catch {
    console.error(FAILURE);
    process.exitCode = 1;
  }
}
