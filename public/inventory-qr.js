/* Asset labels use the local MIT-licensed Project Nayuki QR encoder. */
"use strict";

(function (root) {
    const SVG_NS = "http://www.w3.org/2000/svg";
    let labelSequence = 0;
    let cleanupPrinting = null;

    function t(source, values = []) {
        if (root.PuntoI18n && typeof root.PuntoI18n.t === "function")
            return root.PuntoI18n.t(source, values);
        return source.replace(/\{(\d+)\}/g, (placeholder, index) =>
            index < values.length ? String(values[index]) : placeholder);
    }

    function labelLanguage() {
        return root.PuntoI18n && root.PuntoI18n.locale || "it-IT";
    }

    function assetId(value) {
        const id = typeof value === "string" && /^[1-9][0-9]*$/.test(value)
            ? Number(value) : value;
        if (!Number.isSafeInteger(id) || id < 1)
            throw new Error(t("Identificativo del bene non valido."));
        return id;
    }

    function assetUrl(id, origin = root.location.origin) {
        const base = new URL(origin);
        if (!["http:", "https:"].includes(base.protocol) || base.username || base.password)
            throw new Error(t("Indirizzo dell’app non valido."));
        const url = new URL("/app", base.origin);
        url.hash = `inventory?asset_id=${assetId(id)}`;
        return url.href;
    }

    function element(name, attributes = {}, text) {
        const result = root.document.createElementNS(SVG_NS, name);
        Object.entries(attributes).forEach(([key, value]) => result.setAttribute(key, String(value)));
        if (text !== undefined) result.textContent = text;
        return result;
    }

    function lines(value, width, wordWrap = true) {
        const points = Array.from(value.replace(/\s+/gu, " ").trim());
        const result = [];
        while (points.length) {
            let end = 0;
            let occupied = 0;
            while (end < points.length) {
                // Monospace CJK/emoji glyphs occupy two Latin character cells.
                const wide = /\p{Extended_Pictographic}|[\u1100-\u115f\u2329\u232a\u2e80-\ua4cf\uac00-\ud7a3\uf900-\ufaff\ufe10-\ufe19\ufe30-\ufe6f\uff01-\uff60\uffe0-\uffe6]/u.test(points[end]);
                const cells = wide ? 2 : 1;
                if (occupied + cells > width) break;
                occupied += cells;
                end++;
            }
            if (wordWrap && end < points.length) {
                const lastSpace = points.slice(0, end + 1).lastIndexOf(" ");
                if (lastSpace >= Math.floor(width * 0.8)) end = lastSpace;
            }
            result.push(points.splice(0, end).join(""));
            while (points[0] === " ") points.shift();
        }
        return result.length ? result : [""];
    }

    function createLabel(asset) {
        const id = assetId(asset && asset.id);
        if (typeof asset.asset_tag !== "string" || !asset.asset_tag.trim() ||
            typeof asset.name !== "string" || !asset.name.trim() ||
            new TextEncoder().encode(asset.asset_tag).length > 64 ||
            new TextEncoder().encode(asset.name).length > 160)
            throw new Error(t("Codice o nome del bene non valido."));
        const url = assetUrl(id);
        const library = root.qrcodegen;
        if (!library || !library.QrCode)
            throw new Error(t("Generatore QR non disponibile. Ricaricare la pagina."));
        const qr = library.QrCode.encodeText(url, library.QrCode.Ecc.MEDIUM);
        const identifier = `inventory-qr-label-${++labelSequence}`;
        const svg = element("svg", {
            xmlns: SVG_NS, width: "90mm", height: "50mm", viewBox: "0 0 360 200", lang: labelLanguage(),
            role: "img", "aria-labelledby": `${identifier}-title ${identifier}-description`,
            "font-family": "Arial, sans-serif", "font-size": 11, fill: "#111111"
        });
        svg.append(element("title", {id: `${identifier}-title`},
            t("Etichetta {0}: {1}", [asset.asset_tag, asset.name])));
        svg.append(element("desc", {id: `${identifier}-description`},
            t("QR della scheda del bene {0}. Accesso all’app richiesto. {1}", [id, url])));
        svg.append(element("rect", {width: 360, height: 200, fill: "#ffffff"}));
        const extent = qr.size + 8; // Four white modules on every side (quiet zone).
        const modules = element("g", {transform: `translate(8 8) scale(${162 / extent})`});
        let path = "";
        for (let y = 0; y < qr.size; y++)
            for (let x = 0; x < qr.size; x++)
                if (qr.getModule(x, y)) path += `M${x + 4},${y + 4}h1v1h-1z`;
        modules.append(element("path", {d: path, fill: "#000000", "shape-rendering": "crispEdges"}));
        svg.append(modules);
        svg.append(element("text", {x: 184, y: 18, "font-size": 9, "letter-spacing": 1}, t("PUNTO · ASSET")));
        const tagLines = lines(asset.asset_tag, 21, false);
        tagLines.forEach((line, index) => svg.append(element("text", {
            x: 184, y: 39 + index * 16, "font-size": 13, "font-weight": 700,
            "font-family": "monospace"
        }, line)));
        const nameTop = 39 + (tagLines.length - 1) * 16 + 23;
        const nameLines = lines(asset.name, 28);
        const nameStep = Math.min(13, (187 - nameTop) / Math.max(1, nameLines.length - 1));
        const nameSize = Math.min(10, nameStep * 0.85);
        nameLines.forEach((line, index) => svg.append(element("text", {
            x: 184, y: nameTop + index * nameStep, "font-size": nameSize,
            "font-family": "monospace"
        }, line)));
        svg.append(element("text", {x: 89, y: 183, "text-anchor": "middle", "font-size": 10}, t("Bene #{0}", [id])));
        svg.append(element("text", {x: 89, y: 195, "text-anchor": "middle", "font-size": 8}, t("Accesso all’app richiesto")));
        return svg;
    }

    function downloadLabel(asset) {
        const svg = createLabel(asset);
        const serialized = new XMLSerializer().serializeToString(svg);
        const blob = new Blob(["<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", serialized],
            {type: "image/svg+xml;charset=utf-8"});
        const url = URL.createObjectURL(blob);
        const anchor = root.document.createElement("a");
        anchor.href = url;
        anchor.download = `asset-${assetId(asset.id)}-${asset.asset_tag.replace(/[^a-zA-Z0-9_-]/g, "_").slice(0, 50)}.svg`;
        root.document.body.append(anchor);
        try { anchor.click(); }
        finally {
            anchor.remove();
            root.setTimeout(() => URL.revokeObjectURL(url), 1000);
        }
    }

    function printLabel(asset) {
        const svg = createLabel(asset);
        if (cleanupPrinting) cleanupPrinting();
        const container = root.document.createElement("div");
        container.className = "inventory-qr-print-root";
        container.setAttribute("lang", labelLanguage());
        container.append(svg);
        root.document.body.append(container);
        root.document.body.classList.add("inventory-qr-printing");
        let timer = null;
        const cleanup = () => {
            root.removeEventListener("afterprint", cleanup);
            if (timer !== null) root.clearTimeout(timer);
            container.remove();
            root.document.body.classList.remove("inventory-qr-printing");
            if (cleanupPrinting === cleanup) cleanupPrinting = null;
        };
        cleanupPrinting = cleanup;
        root.addEventListener("afterprint", cleanup, {once: true});
        // Fallback for browsers that omit afterprint when the dialog is cancelled.
        timer = root.setTimeout(cleanup, 300000);
        try { root.print(); }
        catch (error) { cleanup(); throw error; }
    }

    root.InventoryQr = Object.freeze({assetUrl, createLabel, downloadLabel, printLabel});
})(window);
