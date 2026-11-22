(hostModel) => {
  if (window !== window.top || window.origin !== "https://steamloopback.host" ||
      window.opener?.origin !== window.origin) {
    return;
  }

  const installed = Symbol.for("Brinewell.hostCpuName");
  if (document[installed]) return;
  document[installed] = true;

  function showHostModel() {
    if (window.opener.location.pathname !== "/routes/settings") return;
    const localization = window.opener.LocalizationManager;
    if (!localization) return;
    const hardware = localization.LocalizeString("#Settings_System_Hardware");
    const cpuName = localization.LocalizeString("#Settings_System_CPUName");

    const sections = document.querySelectorAll(".DialogSettingsSection[aria-labelledby]");
    const rows = [];
    for (const section of sections) {
      const heading = document.getElementById(section.getAttribute("aria-labelledby"));
      if (heading?.textContent.trim() !== hardware) continue;
      for (const row of section.children) {
        if (row.children.length === 2 && row.firstElementChild.textContent.trim() === cpuName) {
          rows.push(row);
        }
      }
    }
    if (rows.length !== 1) return;

    let value = rows[0].lastElementChild;
    while (value.childElementCount === 1) value = value.firstElementChild;
    const name = value.textContent.trim();
    if (value.childElementCount !== 0 || (name && name !== "Unknown")) return;
    if (value.firstChild?.nodeType === Node.TEXT_NODE && value.firstChild === value.lastChild) {
      value.firstChild.nodeValue = hostModel;
    } else {
      value.textContent = hostModel;
    }
  }

  function observeSettings() {
    new MutationObserver(showHostModel).observe(document.body, {
      childList: true,
      characterData: true,
      subtree: true,
    });
    showHostModel();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", observeSettings, { once: true });
  } else {
    observeSettings();
  }
}
