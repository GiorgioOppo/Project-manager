"use strict";

(async () => {
  if (window.PuntoI18n) await window.PuntoI18n.ready;
  const { t, locale } = window.PuntoI18n || { t: (source, values = []) => source.replace(/\{(\d+)\}/g, (_, index) => values[index]), locale: "it-IT" };
  const form = document.getElementById("login-form");
  const email = document.getElementById("email");
  const password = document.getElementById("password");
  const remember = document.getElementById("remember");
  const toggle = document.getElementById("password-toggle");
  const submit = document.getElementById("login-submit");
  const formError = document.getElementById("form-error");
  const status = document.getElementById("login-status");
  const rememberedEmailKey = "punto.rememberedEmail";
  let submitting = false;
  let localEnabled = false;

  const loadAuthConfig = async () => {
    const authStatus = document.getElementById("auth-status");
    try {
      const response = await fetch("/api/auth/config", {
        credentials: "same-origin", cache: "no-store", signal: AbortSignal.timeout(10000),
      });
      if (!response.ok) throw new Error("auth-config");
      const config = await response.json();
      if (typeof config.local !== "boolean" || typeof config.sso !== "boolean" ||
          (!config.local && !config.sso)) throw new Error("auth-config");
      localEnabled = config.local;
      form.hidden = !config.local;
      document.getElementById("demo-section").hidden = !config.local || config.managed_users === true;
      document.getElementById("sso-signin").hidden = !config.sso;
      document.getElementById("managed-access-note").hidden = config.managed_users !== true;
      authStatus.hidden = true;
    } catch (_) {
      authStatus.textContent = t("Impossibile caricare le modalità di accesso. Ricarica la pagina per riprovare.");
    }
  };
  loadAuthConfig();

  const linkedRoute = /^#ticket-connect\?state=[a-f0-9]{64}(?:&calendar=1)?(?:&time=1)?$/.test(location.hash) || /^#(?:jobs\?job_id|inventory\?asset_id)=[1-9]\d*$/.test(location.hash) || location.hash === "#calendar";
  if (linkedRoute) {
    try { sessionStorage.setItem("punto.ticket.return", JSON.stringify({ hash: location.hash, at: Date.now() })); } catch { /* Il login locale conserva comunque il frammento. */ }
    fetch("/api/session", { credentials: "same-origin", cache: "no-store" }).then((response) => response.json()).then((session) => {
      if (session.authenticated) location.replace("/app" + location.hash);
    }).catch(() => {});
  }

  document.getElementById("current-year").textContent = new Date().getFullYear();

  try {
    const savedEmail = localStorage.getItem(rememberedEmailKey);
    if (savedEmail) {
      email.value = savedEmail;
      remember.checked = true;
    }
  } catch (_) { /* The form remains usable when browser storage is unavailable. */ }

  const setFieldError = (input, message) => {
    const messageElement = document.getElementById(`${input.id}-error`);
    messageElement.textContent = message;
    messageElement.hidden = !message;
    if (message) input.setAttribute("aria-invalid", "true");
    else input.removeAttribute("aria-invalid");
  };

  const clearFormError = () => {
    formError.hidden = true;
    formError.textContent = "";
  };

  for (const input of [email, password]) {
    input.addEventListener("input", () => {
      setFieldError(input, "");
      clearFormError();
    });
  }

  remember.addEventListener("change", () => {
    if (!remember.checked) {
      try { localStorage.removeItem(rememberedEmailKey); } catch (_) { /* Optional storage. */ }
    }
  });

  toggle.addEventListener("click", () => {
    const showing = password.type === "password";
    password.type = showing ? "text" : "password";
    toggle.setAttribute("aria-pressed", String(showing));
    toggle.setAttribute("aria-label", showing ? t("Nascondi password") : t("Mostra password"));
  });

  document.getElementById("demo-fill").addEventListener("click", () => {
    email.value = "demo@punto.local";
    password.value = "Punto2026!";
    setFieldError(email, "");
    setFieldError(password, "");
    clearFormError();
    status.textContent = t("Credenziali demo compilate. Premi Accedi al tuo spazio per continuare.");
    submit.focus();
  });

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (submitting || !localEnabled) return;
    clearFormError();
    email.value = email.value.trim();
    let firstInvalid = null;

    if (!email.value) {
      setFieldError(email, t("Inserisci il tuo indirizzo email."));
      firstInvalid = email;
    } else if (!email.validity.valid) {
      setFieldError(email, t("Inserisci un indirizzo email valido."));
      firstInvalid = email;
    } else setFieldError(email, "");

    if (!password.value) {
      setFieldError(password, t("Inserisci la tua password."));
      firstInvalid ||= password;
    } else setFieldError(password, "");

    if (firstInvalid) {
      firstInvalid.focus();
      return;
    }

    submitting = true;
    submit.disabled = true;
    submit.classList.add("is-loading");
    submit.querySelector(".button-label").textContent = t("Accesso in corso…");
    form.setAttribute("aria-busy", "true");
    status.textContent = t("Accesso in corso.");
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 15000);
    const body = new URLSearchParams({ email: email.value, password: password.value });
    if (remember.checked) body.set("remember", "on");

    try {
      const response = await fetch("/api/login", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        credentials: "same-origin",
        body: body.toString(),
        signal: controller.signal,
      });
      const data = await response.json();
      if (!response.ok || !data.ok) {
        const message = typeof data.error === "string" && data.error.trim()
          ? t(data.error) : t("Email o password non corrette. Riprova.");
        formError.textContent = message;
        formError.hidden = false;
        status.textContent = t("Accesso non riuscito.");
        password.focus();
        return;
      }
      try {
        if (remember.checked) localStorage.setItem(rememberedEmailKey, email.value);
        else localStorage.removeItem(rememberedEmailKey);
      } catch (_) { /* Saving an email is optional and must not block login. */ }
      status.textContent = t("Accesso completato. Apertura del tuo spazio.");
      const reportHash = (/^#analisi\/(?:periodo|commesse|utenti|costi|costo-ore)(?:\?|$)/.test(window.location.hash) || /^#ticket-connect\?state=[a-f0-9]{64}(?:&calendar=1)?(?:&time=1)?$/.test(window.location.hash) || /^#(?:jobs\?job_id|inventory\?asset_id)=[1-9]\d*$/.test(window.location.hash) || window.location.hash === "#calendar")
        && window.location.hash.length <= 2048 ? window.location.hash : "";
      window.location.assign(window.PuntoI18n ? window.PuntoI18n.url("/app" + reportHash) : "/app" + reportHash);
    } catch (error) {
      formError.textContent = error.name === "AbortError"
        ? t("La richiesta sta impiegando troppo tempo. Riprova tra poco.")
        : t("Non riusciamo a connetterci. Controlla la connessione e riprova.");
      formError.hidden = false;
      status.textContent = t("Accesso non riuscito.");
    } finally {
      clearTimeout(timeout);
      submitting = false;
      submit.disabled = false;
      submit.classList.remove("is-loading");
      submit.querySelector(".button-label").textContent = t("Accedi al tuo spazio");
      form.removeAttribute("aria-busy");
    }
  });
})();
