// TeddyBox plugin for teddyCloud
(function () {
  "use strict";

  const PLUGIN_VERSION = "0.1.1";
  const REPO = "Sandbox-Vibes/teddybox";
  const $ = (id) => document.getElementById(id);

  /* German if the browser prefers German, else English (like the box's setup page) */
  const DE = /^de\b/i.test((navigator.languages && navigator.languages[0]) || navigator.language || "");
  const DE_TEXT = {
    intro: "Eigene Firmware für die ESP32-Toniebox. Diese Seite zeigt, welche Boxen sie nutzen, und hilft, Audio auf einen Tonie zu bringen.",
    Boxes: "Boxen", Reload: "Neu laden", Box: "Box", Firmware: "Firmware", "Last connection": "Letzte Verbindung", "Setup page": "Einrichtungsseite",
    "Loading …": "Lade …",
    setupHint: "Die Einrichtungsseite antwortet nur im Einrichtungsmodus: Home-Assistant-Knopf „Einrichtung starten“ oder großes Ohr beim Einschalten halten.",
    "Audio to tonie": "Audio auf einen Tonie",
    audioHint: "Die Audiodateien werden in die Bibliothek von teddyCloud geladen, dort zu einer TAF-Datei umgewandelt (ein Kapitel pro Datei, nach Namen sortiert) und dem Tonie als Inhalt zugewiesen. Die Box lädt sie beim nächsten Auflegen oder beim nächsten Abgleich.",
    Tonie: "Tonie", Title: "Titel", "Audio files": "Audiodateien", "Convert and assign": "Umwandeln und zuweisen",
    ownHint: "Auf der TeddyBox behält ein Tonie, dessen Datei auf der Einrichtungsseite hochgeladen wurde, diese Datei (geschützt). Den Schutz dort zuerst aufheben.",
    "Live events": "Live-Ereignisse", Start: "Starten", Stop: "Anhalten",
    eventsHint: "Was die Boxen gerade an teddyCloud melden: Tonie aufgelegt, Ohren, Lautstärke, Wiedergabe. teddyCloud sagt nicht, von welcher Box ein Ereignis kommt.",
    Releases: "Versionen", disclaimer: "Inoffizielles Projekt, nicht mit tonies GmbH verbunden.",
    "Latest release: {v}": "Neueste Version: {v}", "Latest release unknown (no internet?)": "Neueste Version unbekannt (kein Internet?)",
    "No boxes known to teddyCloud yet.": "teddyCloud kennt noch keine Box.", "Could not load the boxes: {e}": "Boxen konnten nicht geladen werden: {e}",
    "original or unknown": "Original oder unbekannt", "up to date": "aktuell", "update: {v}": "Update: {v}", "development build": "Entwicklungsstand",
    never: "nie", "{n} min ago": "vor {n} min", "{n} h ago": "vor {n} h", "{n} days ago": "vor {n} Tagen",
    "– choose –": "– wählen –", "(no name)": "(ohne Namen)",
    "Please choose a tonie.": "Bitte einen Tonie wählen.", "Please choose audio files.": "Bitte Audiodateien wählen.", "Please enter a title.": "Bitte einen Titel eingeben.",
    "Uploading {n} file(s) …": "Lade {n} Datei(en) hoch …", "Encoding in teddyCloud …": "Wandle in teddyCloud um …", "Assigning to the tonie …": "Weise dem Tonie zu …",
    "Done: {f} is now the content of {t}.": "Fertig: {f} ist jetzt der Inhalt von {t}.",
    "Failed: {e}": "Fehlgeschlagen: {e}", "teddyCloud asks for a login (API access denied).": "teddyCloud verlangt eine Anmeldung (Zugriff auf die API verweigert).",
    "connected": "verbunden", "connection lost, retrying": "Verbindung unterbrochen, versuche erneut",
    "Replace the content of {t}?": "Inhalt von {t} ersetzen?",
  };
  function T(s, v) {
    let r = DE ? (DE_TEXT[s] ?? s) : s;
    if (v) for (const k in v) r = r.split("{" + k + "}").join(v[k]);
    return r;
  }
  function translatePage() {
    document.documentElement.lang = DE ? "de" : "en";
    document.querySelectorAll("[data-t]").forEach((e) => (e.textContent = T(e.dataset.t)));
  }
  function setStatus(el, msg, cls) {
    el.textContent = msg;
    el.className = "status " + (cls || "");
  }
  function esc(s) {
    return String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
  }

  /* teddyCloud API (same origin) */
  async function api(path, opts) {
    const r = await fetch(path, Object.assign({ credentials: "same-origin" }, opts || {}));
    if (r.status === 401 || r.status === 403) throw new Error(T("teddyCloud asks for a login (API access denied)."));
    if (!r.ok) throw new Error("HTTP " + r.status + " " + path.split("?")[0]);
    return r;
  }
  const getJson = async (p) => (await api(p)).json();
  const getText = async (p) => (await (await api(p)).text()).trim();
  function form(obj) {
    const p = new URLSearchParams();
    for (const [k, v] of obj) p.append(k, v);
    return p.toString();
  }

  /* versions: "v0.5.0", "v0.5.0-3-g48239dd" (3 commits after v0.5.0) */
  function parseVer(s) {
    const m = /v?(\d+)\.(\d+)\.(\d+)(?:-(\d+)-g[0-9a-f]+)?/.exec(s || "");
    return m ? { n: [+m[1], +m[2], +m[3]], ahead: +(m[4] || 0) } : null;
  }
  function cmpVer(a, b) {
    for (let i = 0; i < 3; i++) if (a.n[i] !== b.n[i]) return a.n[i] - b.n[i];
    return 0;
  }
  function ago(ts) {
    if (!ts) return T("never");
    const s = Date.now() / 1000 - ts;
    if (s < 3600) return T("{n} min ago", { n: Math.max(1, Math.round(s / 60)) });
    if (s < 86400) return T("{n} h ago", { n: Math.round(s / 3600) });
    return T("{n} days ago", { n: Math.round(s / 86400) });
  }

  let latest = null; /* {tag, url} */
  async function loadLatest() {
    try {
      const r = await fetch("https://api.github.com/repos/" + REPO + "/releases/latest");
      if (!r.ok) throw new Error();
      const j = await r.json();
      latest = { tag: j.tag_name, url: j.html_url };
      $("latest").innerHTML = esc(T("Latest release: {v}", { v: j.tag_name })) + ' · <a href="' + esc(j.html_url) + '" target="_blank" rel="noopener">GitHub</a>';
    } catch (e) {
      latest = null;
      $("latest").textContent = T("Latest release unknown (no internet?)");
    }
  }

  let boxes = [];
  async function loadBoxes() {
    const tb = $("boxes");
    try {
      const j = await getJson("/api/getBoxes");
      boxes = j.boxes || [];
      if (!boxes.length) {
        tb.innerHTML = '<tr><td colspan="4" class="muted">' + esc(T("No boxes known to teddyCloud yet.")) + "</td></tr>";
        fillBoxSelect();
        return;
      }
      for (const b of boxes) {
        const ov = "?overlay=" + encodeURIComponent(b.ID);
        /* TeddyBox reports "TeddyBox vX.Y.Z" over RTNL (newer teddyCloud stores it);
           its user agent "toniebox-esp32-teddybox/vX.Y.Z" ends up as "teddybox/vX.Y.Z" */
        b.fw = await getText("/api/settings/get/internal.toniebox_firmware.rtnlVersion" + ov).catch(() => "");
        if (!/^TeddyBox\b/.test(b.fw)) {
          const ua = await getText("/api/settings/get/internal.toniebox_firmware.uaEsp32Firmware" + ov).catch(() => "");
          if (/^teddybox\//i.test(ua)) b.fw = "TeddyBox " + ua.slice(9);
          else if (!b.fw && ua) b.fw = ua;
        }
        b.last = +(await getText("/api/settings/get/internal.last_connection" + ov).catch(() => "0")) || 0;
      }
      tb.innerHTML = "";
      for (const b of boxes) {
        const isTb = /^TeddyBox\b/.test(b.fw);
        let fw;
        if (isTb) {
          const v = b.fw.replace(/^TeddyBox\s*/, "");
          const mine = parseVer(v), rel = latest && parseVer(latest.tag);
          let tag = "";
          if (mine && rel) {
            const c = cmpVer(mine, rel);
            if (c < 0) tag = '<span class="tag warn">' + esc(T("update: {v}", { v: latest.tag })) + "</span>";
            else if (c === 0 && !mine.ahead) tag = '<span class="tag ok">' + esc(T("up to date")) + "</span>";
            else tag = '<span class="tag">' + esc(T("development build")) + "</span>";
          }
          fw = "TeddyBox <span class=\"mono\">" + esc(v) + "</span> " + tag;
        } else {
          fw = '<span class="muted">' + esc(b.fw || T("original or unknown")) + "</span>";
        }
        /* host name of the box in the home network: teddybox-<last 4 hex digits of its MAC> */
        const id = String(b.commonName || b.ID || "").toLowerCase().replace(/[^0-9a-f]/g, "");
        const host = id.length >= 4 ? "teddybox-" + id.slice(-4) + ".local" : "";
        const link = isTb && host ? '<a href="http://' + host + '/" target="_blank" rel="noopener" class="mono">' + host + "</a>" : '<span class="muted">–</span>';
        const tr = document.createElement("tr");
        tr.innerHTML =
          "<td>" + esc(b.boxName || b.ID) + '<br><span class="muted mono">' + esc(b.ID) + "</span></td><td>" + fw + "</td><td>" + esc(ago(b.last)) + "</td><td>" + link + "</td>";
        tb.append(tr);
      }
      fillBoxSelect();
    } catch (e) {
      tb.innerHTML = '<tr><td colspan="4">' + esc(T("Could not load the boxes: {e}", { e: e.message })) + "</td></tr>";
    }
  }

  /* audio to tonie */
  function fillBoxSelect() {
    const s = $("abox"), keep = s.value;
    s.innerHTML = "";
    s.add(new Option(T("– choose –"), ""));
    boxes.forEach((b) => s.add(new Option((b.boxName || b.ID) + (/^TeddyBox\b/.test(b.fw) ? " (TeddyBox)" : ""), b.ID)));
    if (keep) s.value = keep;
    else if (boxes.length === 1) s.value = boxes[0].ID;
    loadTonies();
  }
  let tonies = [];
  function tonieLabel(t) {
    const i = t.tonieInfo || {};
    const name = [i.series, i.episode].filter(Boolean).join(" – ");
    return (name || T("(no name)")) + " · " + t.uid;
  }
  async function loadTonies() {
    const s = $("atonie");
    s.innerHTML = "";
    s.add(new Option(T("– choose –"), ""));
    const box = $("abox").value;
    if (!box) return;
    try {
      const j = await getJson("/api/getTagIndex?overlay=" + encodeURIComponent(box));
      tonies = (j.tags || []).filter((t) => t.type === "tag" && !t.hide);
      tonies.sort((a, b) => tonieLabel(a).localeCompare(tonieLabel(b), DE ? "de" : "en"));
      tonies.forEach((t) => s.add(new Option(tonieLabel(t), t.ruid)));
    } catch (e) {
      setStatus($("astatus"), T("Failed: {e}", { e: e.message }), "err");
    }
  }
  function sortedFiles() {
    return [...$("afiles").files].sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }));
  }
  function showFiles() {
    const l = $("alist");
    l.innerHTML = "";
    sortedFiles().forEach((f) => {
      const li = document.createElement("li");
      li.textContent = f.name + " (" + (f.size / 1048576).toFixed(1) + " MB)";
      l.append(li);
    });
    if (!$("atitle").value && $("afiles").files.length) {
      $("atitle").value = sortedFiles()[0].name.replace(/\.[^.]+$/, "");
    }
  }
  function slug(s) {
    return (s.normalize("NFKD").replace(/[̀-ͯ]/g, "").replace(/[^A-Za-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 40) || "audio");
  }
  function upload(path, files) {
    return new Promise((resolve, reject) => {
      const fd = new FormData();
      files.forEach((f, i) => fd.append("file", f, String(i + 1).padStart(2, "0") + "_" + slug(f.name.replace(/\.[^.]+$/, "")) + (/\.[^.]+$/.exec(f.name) || [""])[0]));
      const x = new XMLHttpRequest();
      x.open("POST", "/api/fileUpload?special=library&path=" + encodeURIComponent(path));
      x.upload.onprogress = (e) => { if (e.lengthComputable) $("aprog").value = Math.round((e.loaded * 90) / e.total); };
      x.onload = () => (x.status === 200 ? resolve() : reject(new Error(x.status === 401 ? T("teddyCloud asks for a login (API access denied).") : "upload: HTTP " + x.status + " " + x.responseText.slice(0, 120))));
      x.onerror = () => reject(new Error("upload"));
      x.send(fd);
    });
  }
  async function convert() {
    const st = $("astatus"), box = $("abox").value, ruid = $("atonie").value, files = sortedFiles(), title = $("atitle").value.trim();
    if (!box || !ruid) return setStatus(st, T("Please choose a tonie."), "err");
    if (!files.length) return setStatus(st, T("Please choose audio files."), "err");
    if (!title) return setStatus(st, T("Please enter a title."), "err");
    const tonie = tonies.find((t) => t.ruid === ruid);
    if (tonie && tonie.source && !confirm(T("Replace the content of {t}?", { t: tonieLabel(tonie) }))) return;
    const stamp = new Date().toISOString().slice(0, 16).replace(/[-:T]/g, "");
    const base = "teddybox/" + slug(title) + "-" + stamp;
    $("ago").disabled = true;
    $("aprog").hidden = false;
    $("aprog").value = 0;
    try {
      setStatus(st, T("Uploading {n} file(s) …", { n: files.length }));
      await api("/api/dirCreate?special=library", { method: "POST", body: "teddybox" }).catch(() => {});
      await api("/api/dirCreate?special=library", { method: "POST", body: base });
      await upload(base, files);
      setStatus(st, T("Encoding in teddyCloud …"));
      const names = files.map((f, i) => base + "/" + String(i + 1).padStart(2, "0") + "_" + slug(f.name.replace(/\.[^.]+$/, "")) + (/\.[^.]+$/.exec(f.name) || [""])[0]);
      const body = form([["target", base + ".taf"], ...names.map((n) => ["source", n])]);
      await api("/api/fileEncode?special=library", { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body });
      $("aprog").value = 95;
      setStatus(st, T("Assigning to the tonie …"));
      await api("/content/json/set/" + ruid + "?overlay=" + encodeURIComponent(box), {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: form([["source", "lib://" + base + ".taf"]]),
      });
      $("aprog").value = 100;
      setStatus(st, T("Done: {f} is now the content of {t}.", { f: base + ".taf", t: tonie ? tonieLabel(tonie) : ruid }), "ok");
      loadTonies();
    } catch (e) {
      setStatus(st, T("Failed: {e}", { e: e.message }), "err");
    }
    $("ago").disabled = false;
  }

  /* live events (Server-Sent Events of teddyCloud) */
  let es = null;
  function logEvent(line) {
    const p = $("events");
    const t = new Date().toLocaleTimeString(DE ? "de-DE" : "en-GB");
    p.textContent = (t + "  " + line + "\n" + p.textContent).split("\n").slice(0, 200).join("\n");
  }
  function toggleEvents() {
    if (es) {
      es.close();
      es = null;
      $("evtoggle").textContent = T("Start");
      return;
    }
    es = new EventSource("/api/sse");
    $("evtoggle").textContent = T("Stop");
    es.onopen = () => logEvent("— " + T("connected"));
    es.onerror = () => logEvent("— " + T("connection lost, retrying"));
    es.onmessage = (ev) => handle(ev.data);
    ["pressed", "charger", "ContentAudioId", "ContentTitle", "TagValid", "TagInvalid", "knock", "tilt", "playback", "cmd", "VolumeLevel", "VolumedB", "BoxTilt-A", "BoxTilt-B"].forEach((n) =>
      es.addEventListener(n, (ev) => handle(ev.data))
    );
  }
  function handle(data) {
    try {
      const j = JSON.parse(data);
      if (j.type === "keep-alive") return;
      logEvent(j.type + ": " + (typeof j.data === "string" ? j.data : JSON.stringify(j.data)));
    } catch (e) {
      if (data) logEvent(String(data).slice(0, 200));
    }
  }

  async function init() {
    translatePage();
    $("pluginVersion").textContent = "(v" + PLUGIN_VERSION + ")";
    $("reload").onclick = async () => { await loadLatest(); await loadBoxes(); };
    $("abox").onchange = loadTonies;
    $("afiles").onchange = showFiles;
    $("ago").onclick = convert;
    $("evtoggle").onclick = toggleEvents;
    await loadLatest();
    await loadBoxes();
  }
  if (document.readyState !== "loading") init();
  else document.addEventListener("DOMContentLoaded", init);
})();
