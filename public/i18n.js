"use strict";

// This module translates marked interface text only. Record names, notes and API
// identifiers must never be passed through a document-wide text replacement.
(() => {
  const languages = { it: "Italiano", en: "English", zh: "简体中文", es: "Español", fr: "Français", de: "Deutsch" };
  const locales = { it: "it-IT", en: "en-GB", zh: "zh-CN", es: "es-ES", fr: "fr-FR", de: "de-DE" };
  const core = {
    it: ["Lingua", "Cambiare lingua? La pagina verrà ricaricata e le modifiche non salvate andranno perse.", "Traduzioni non disponibili. L’interfaccia è in italiano; ricarica la pagina per riprovare."],
    en: ["Language", "Change language? The page will reload and unsaved changes will be lost.", "Translations are unavailable. The interface is in Italian; reload the page to try again."],
    zh: ["语言", "要切换语言吗？页面将重新加载，未保存的更改将丢失。", "翻译暂不可用。界面已使用意大利语；请重新加载页面重试。"],
    es: ["Idioma", "¿Cambiar de idioma? La página se recargará y se perderán los cambios sin guardar.", "Las traducciones no están disponibles. La interfaz está en italiano; recarga la página para reintentar."],
    fr: ["Langue", "Changer de langue ? La page sera rechargée et les modifications non enregistrées seront perdues.", "Les traductions sont indisponibles. L’interface est en italien ; rechargez la page pour réessayer."],
    de: ["Sprache", "Sprache wechseln? Die Seite wird neu geladen und ungespeicherte Änderungen gehen verloren.", "Übersetzungen sind nicht verfügbar. Die Oberfläche ist auf Italienisch; laden Sie die Seite erneut, um es nochmals zu versuchen."]
  };
  const storageKey = "punto.language";
  const normalize = (value) => {
    const key = String(value || "").toLowerCase().split(/[-_]/)[0];
    return Object.hasOwn(languages, key) ? key : null;
  };
  const savedLanguage = () => {
    for (const storage of ["localStorage", "sessionStorage"]) {
      try { const value = normalize(window[storage].getItem(storageKey)); if (value) return value; } catch (_) { /* Storage is optional. */ }
    }
    return null;
  };
  const requested = normalize(new URL(location.href).searchParams.get("lang")) || savedLanguage() ||
    (navigator.languages || [navigator.language]).map(normalize).find(Boolean) || "it";
  let language = requested;
  let messages = Object.create(null);
  const dirtyForms = new Set();
  const saveLanguage = (value) => {
    for (const storage of ["localStorage", "sessionStorage"]) {
      try { window[storage].setItem(storageKey, value); } catch (_) { /* URL also preserves this choice. */ }
    }
  };
  if (new URL(location.href).searchParams.has("lang")) saveLanguage(requested);

  function t(source, values = []) {
    const key = String(source ?? "");
    const translated = Object.hasOwn(messages, key) ? messages[key] : key;
    // A callback keeps dollar signs and markup in values literal. No HTML is evaluated.
    return translated.replace(/\{(\d+)\}/g, (placeholder, index) => index < values.length ? String(values[index]) : placeholder);
  }

  function translateDocument() {
    document.documentElement.lang = locales[language];
    for (const element of document.querySelectorAll("[data-i18n]")) {
      for (const child of element.childNodes) {
        if (child.nodeType !== 3 || !child.textContent.trim()) continue;
        const original = child.textContent;
        const key = original.trim();
        // Whitespace around inline icons and links is part of the layout.
        child.textContent = original.slice(0, original.indexOf(key)) + t(key) + original.slice(original.indexOf(key) + key.length);
      }
    }
    for (const element of document.querySelectorAll("[data-i18n-attrs]")) {
      for (const attribute of element.dataset.i18nAttrs.split(" ")) {
        if (element.hasAttribute(attribute)) element.setAttribute(attribute, t(element.getAttribute(attribute)));
      }
    }
  }

  function localizedURL(value) {
    const url = new URL(value, location.href);
    url.searchParams.set("lang", language);
    return url.href;
  }

  function installSelector(failed) {
    const selector = document.getElementById("language-select");
    if (!selector) return;
    selector.replaceChildren(...Object.entries(languages).map(([value, label]) => new Option(label, value)));
    selector.value = language;
    selector.setAttribute("aria-label", core[language][0]);
    document.getElementById("language-label").textContent = core[language][0];
    selector.addEventListener("change", () => {
      const next = normalize(selector.value);
      const unsaved = [...dirtyForms].some((form) => form.isConnected && !form.closest("[hidden]") && (!form.closest("dialog") || form.closest("dialog").open));
      if (!next || next === language || unsaved && !window.confirm(core[language][1])) { selector.value = language; return; }
      saveLanguage(next);
      const url = new URL(location.href);
      url.searchParams.set("lang", next);
      location.assign(url.href);
    });
    document.addEventListener("input", (event) => { if (event.target.form) dirtyForms.add(event.target.form); });
    document.addEventListener("change", (event) => { if (event.target.form) dirtyForms.add(event.target.form); });
    document.addEventListener("reset", (event) => dirtyForms.delete(event.target));
    if (failed) {
      const notice = document.createElement("p");
      notice.className = "language-error";
      notice.setAttribute("role", "status");
      notice.textContent = core[requested][2];
      selector.closest("header").after(notice);
    }
  }

  const api = { t, url: localizedURL, markSaved: (form) => dirtyForms.delete(form), get language() { return language; }, get locale() { return locales[language]; } };
  window.PuntoI18n = api;
  api.ready = (async () => {
    let failed = false;
    if (language !== "it") {
      try {
        const response = await fetch(`/locales/${language}.json`, { credentials: "same-origin", cache: "no-store", signal: AbortSignal.timeout(10000) });
        if (!response.ok) throw new Error("catalog");
        const catalog = await response.json();
        if (!catalog || Array.isArray(catalog) || typeof catalog !== "object" || Object.values(catalog).some((value) => typeof value !== "string")) throw new Error("catalog");
        messages = catalog;
      } catch (_) { language = "it"; failed = true; }
    }
    translateDocument();
    installSelector(failed);
  })();
})();
