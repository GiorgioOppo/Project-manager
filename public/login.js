"use strict";

(() => {
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
    toggle.setAttribute("aria-label", showing ? "Nascondi password" : "Mostra password");
  });

  document.getElementById("demo-fill").addEventListener("click", () => {
    email.value = "demo@punto.local";
    password.value = "Punto2026!";
    setFieldError(email, "");
    setFieldError(password, "");
    clearFormError();
    status.textContent = "Credenziali demo compilate. Premi Accedi al tuo spazio per continuare.";
    submit.focus();
  });

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (submitting) return;
    clearFormError();
    email.value = email.value.trim();
    let firstInvalid = null;

    if (!email.value) {
      setFieldError(email, "Inserisci il tuo indirizzo email.");
      firstInvalid = email;
    } else if (!email.validity.valid) {
      setFieldError(email, "Inserisci un indirizzo email valido.");
      firstInvalid = email;
    } else setFieldError(email, "");

    if (!password.value) {
      setFieldError(password, "Inserisci la tua password.");
      firstInvalid ||= password;
    } else setFieldError(password, "");

    if (firstInvalid) {
      firstInvalid.focus();
      return;
    }

    submitting = true;
    submit.disabled = true;
    submit.classList.add("is-loading");
    submit.querySelector(".button-label").textContent = "Accesso in corso…";
    form.setAttribute("aria-busy", "true");
    status.textContent = "Accesso in corso.";
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
          ? data.error : "Email o password non corrette. Riprova.";
        formError.textContent = message;
        formError.hidden = false;
        status.textContent = "Accesso non riuscito.";
        password.focus();
        return;
      }
      try {
        if (remember.checked) localStorage.setItem(rememberedEmailKey, email.value);
        else localStorage.removeItem(rememberedEmailKey);
      } catch (_) { /* Saving an email is optional and must not block login. */ }
      status.textContent = "Accesso completato. Apertura del tuo spazio.";
      window.location.assign("/app");
    } catch (error) {
      formError.textContent = error.name === "AbortError"
        ? "La richiesta sta impiegando troppo tempo. Riprova tra poco."
        : "Non riusciamo a connetterci. Controlla la connessione e riprova.";
      formError.hidden = false;
      status.textContent = "Accesso non riuscito.";
    } finally {
      clearTimeout(timeout);
      submitting = false;
      submit.disabled = false;
      submit.classList.remove("is-loading");
      submit.querySelector(".button-label").textContent = "Accedi al tuo spazio";
      form.removeAttribute("aria-busy");
    }
  });
})();
