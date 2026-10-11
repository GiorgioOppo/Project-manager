FROM jc21/nginx-proxy-manager:2.16.0

# The upstream API listens on every interface. Keep the pinned version's
# management API local to Nginx; fail the build if that source changes.
RUN node --input-type=module -e 'import fs from "node:fs"; const path="/app/index.js"; const source=fs.readFileSync(path,"utf8"); const before="app.listen(3000, () =>"; if(source.split(before).length!==2) throw new Error("Unexpected NPM API entrypoint"); fs.writeFileSync(path,source.replace(before,"app.listen(3000, \"127.0.0.1\", () =>"));'

COPY admin.conf /etc/nginx/conf.d/production.conf
# The upstream prepare hook recreates production.conf from this template.
COPY admin.conf /etc/nginx/conf.d/production.conf.template
COPY log-proxy.conf /etc/nginx/conf.d/include/log-proxy.conf

# Protect existing hosts before Nginx starts and every future NPM regeneration.
# The callback location is cloned from the rendered root, retaining its ACL.
COPY proxy-privacy.mjs /app/punto-proxy-privacy.mjs
COPY patch-privacy.mjs /tmp/punto-patch-privacy.mjs
RUN node /tmp/punto-patch-privacy.mjs && rm /tmp/punto-patch-privacy.mjs
