"use strict";

// Callback parameters are temporary credentials, never page content. Remove
// them before language changes or navigation can reuse the callback address.
if (window.location.pathname === "/auth/callback" || window.location.pathname === "/auth/login") {
  window.history.replaceState(null, "", "/auth/error");
}

(async () => {
  const year = document.getElementById("current-year");
  if (year) year.textContent = new Date().getFullYear();
  if (window.PuntoI18n) await window.PuntoI18n.ready;
  const t = window.PuntoI18n?.t || ((source) => source);
  const back = document.getElementById("auth-error-back");
  if (back && window.PuntoI18n) back.href = window.PuntoI18n.url("/");
  const logout = document.getElementById("sso-error-logout");
  const status = document.getElementById("sso-error-status");
  if (!logout || !status) return;
  const label = logout.querySelector("span");
  logout.disabled = false;
  logout.addEventListener("click", async () => {
    logout.disabled = true;
    label.textContent = t("Uscita dall’account…");
    status.hidden = true;
    try {
      const response = await fetch("/auth/logout", {
        method: "POST", credentials: "same-origin", cache: "no-store",
        redirect: "error", headers: { Accept: "application/json" }, body: ""
      });
      if (!response.ok) throw new Error("logout");
      const result = await response.json();
      if (result?.ok !== true || typeof result.logout_url !== "string") throw new Error("logout");
      const destination = new URL(result.logout_url);
      if (destination.protocol !== "https:" && destination.protocol !== "http:") throw new Error("logout");
      window.location.assign(destination.href);
    } catch (_) {
      label.textContent = t("Esci dall’account aziendale");
      status.textContent = t("Impossibile uscire dall’account. Riprova.");
      status.hidden = false;
      logout.disabled = false;
    }
  });
})();
