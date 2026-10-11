/* Deliberately match the pinned image's exact source shape. An upstream
 * change must fail image construction instead of silently dropping privacy. */
import fs from "node:fs";

const nginxPath = "/app/internal/nginx.js";
let nginx = fs.readFileSync(nginxPath, "utf8");
const anchor = "fs.writeFileSync(filename, config_text, { encoding: \"utf8\" });";
if (nginx.split(anchor).length !== 3) throw new Error("Unexpected pinned NPM config writer");
nginx = 'import { protectProxyConfig } from "../punto-proxy-privacy.mjs";\n' + nginx;
/* The first writer generates proxy/default/redirect/stream hosts. The second
 * writes a temporary LetsEncrypt challenge, which must remain untouched. */
nginx = nginx.replace(anchor, 'fs.writeFileSync(filename, nice_host_type === "proxy_host" ? protectProxyConfig(config_text) : config_text, { encoding: "utf8" });');
fs.writeFileSync(nginxPath, nginx);

const preparePath = "/etc/s6-overlay/s6-rc.d/prepare/00-all.sh";
let prepare = fs.readFileSync(preparePath, "utf8");
const before = ". /etc/s6-overlay/s6-rc.d/prepare/90-banner.sh";
if (prepare.split(before).length !== 2) throw new Error("Unexpected pinned NPM preparation hook");
prepare = prepare.replace(before, 'node /app/punto-proxy-privacy.mjs\n' + before);
fs.writeFileSync(preparePath, prepare);
