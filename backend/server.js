"use strict";
const express = require("express");
const { chromium } = require("playwright");
const dgram = require("dgram");
const os = require("os");
const crypto = require("crypto");
const path = require("path");
const fs = require("fs");
const { Readable } = require("stream");

const HTTP_PORT = Number(process.env.PBDL_HTTP_PORT || 32124);
const DISCOVERY_PORT = Number(process.env.PBDL_DISCOVERY_PORT || 32123);
const VIEWPORT = { width: 1280, height: 720 };

let browser = null;
let context = null;
let page = null;
let lastDownload = null;
let lastMessage = "Inicializando Chromium...";
const downloads = new Map();
const recentRequests = new Map();

function safeName(name) {
  name = String(name || "download.bin").replace(/[\\/:*?"<>|]/g, "_").trim();
  return name || "download.bin";
}
function guessName(url, fallback) {
  try {
    const u = new URL(url);
    const n = decodeURIComponent(u.pathname.split("/").filter(Boolean).pop() || "");
    return safeName(n || fallback || "download.bin");
  } catch (_) {
    return safeName(fallback || "download.bin");
  }
}
function isPkg(name) { return /\.pkg$/i.test(String(name || "")); }

function registerRemote(url, name, reqInfo, contentType) {
  const token = crypto.randomUUID();
  const filename = safeName(name || guessName(url, "download.bin"));
  const kind = isPkg(filename) ? "pkg" : "file";
  downloads.set(token, {
    token: token,
    kind: kind,
    url: url,
    filename: filename,
    requestInfo: reqInfo || null,
    contentType: contentType || "application/octet-stream",
    referer: page ? page.url() : "",
    localPath: null,
    ready: true
  });
  lastDownload = {
    token: token,
    kind: kind,
    filename: filename,
    handoff: "/handoff/" + kind + "/" + token + "/" + encodeURIComponent(filename)
  };
  lastMessage = "Download pronto para enviar ao PS4: " + filename;
}

async function registerDownload(download) {
  const url = download.url();
  const filename = safeName(download.suggestedFilename() || guessName(url, "download.bin"));
  const token = crypto.randomUUID();
  const kind = isPkg(filename) ? "pkg" : "file";
  const dir = path.join(os.tmpdir(), "ps4-browser-hybrid");
  fs.mkdirSync(dir, { recursive: true });
  const localPath = path.join(dir, token + "-" + filename);

  const meta = {
    token: token, kind: kind, url: url, filename: filename,
    requestInfo: recentRequests.get(url) || null,
    contentType: "application/octet-stream",
    referer: page ? page.url() : "", localPath: localPath, ready: false
  };
  downloads.set(token, meta);

  // Important for one-time/temporary hoster URLs: do NOT cancel and re-fetch.
  // Let the modern Chromium session consume the real download using its own
  // cookies, JS challenges, POST body and tokens, then serve that local file
  // to the PS4 with Range support.
  lastDownload = {
    token: token, kind: kind, filename: filename,
    preparing: true, handoff: null
  };
  lastMessage = "Chromium esta baixando no PC: " + filename;

  try {
    await download.saveAs(localPath);
    meta.ready = true;
    lastDownload = {
      token: token, kind: kind, filename: filename, preparing: false,
      handoff: "/handoff/" + kind + "/" + token + "/" + encodeURIComponent(filename)
    };
    lastMessage = "Arquivo pronto para enviar ao PS4: " + filename;
  } catch (e) {
    lastDownload = null;
    lastMessage = "Falha no download Chromium: " + String(e.message || e);
  }
}

function getLanIp() {
  const nets = os.networkInterfaces();
  const lists = Object.values(nets);
  for (let i = 0; i < lists.length; i++) {
    const list = lists[i] || [];
    for (let j = 0; j < list.length; j++) {
      const n = list[j];
      if (n && n.family === "IPv4" && !n.internal && !n.address.startsWith("169.254."))
        return n.address;
    }
  }
  return "127.0.0.1";
}

async function ensurePage() {
  if (page && !page.isClosed()) return page;

  if (!browser) {
    browser = await chromium.launch({
      headless: true,
      args: ["--disable-dev-shm-usage", "--disable-background-timer-throttling"]
    });
  }
  if (!context) {
    context = await browser.newContext({ viewport: VIEWPORT, acceptDownloads: true });
  }
  page = await context.newPage();
  page.setDefaultNavigationTimeout(35000);

  page.on("request", function(req) {
    recentRequests.set(req.url(), {
      method: req.method(),
      headers: req.headers(),
      postData: req.postData()
    });
    if (recentRequests.size > 300) {
      recentRequests.delete(recentRequests.keys().next().value);
    }
  });

  page.on("download", function(d) {
    registerDownload(d).catch(function(e) {
      lastMessage = "Erro no download: " + String(e.message || e);
    });
  });

  page.on("dialog", async function(d) {
    try { await d.accept(); } catch (_) {}
  });

  page.on("popup", async function(popup) {
    try {
      await popup.waitForLoadState("domcontentloaded", { timeout: 15000 }).catch(function(){});
      const u = popup.url();
      if (u && u !== "about:blank") {
        await popup.close().catch(function(){});
        await page.goto(u, { waitUntil: "domcontentloaded" });
      }
    } catch (_) {}
  });

  try {
    await page.goto("https://www.google.com/", { waitUntil: "domcontentloaded", timeout: 35000 });
    lastMessage = "Chromium pronto.";
  } catch (e) {
    lastMessage = "Google falhou: " + String(e.message || e);
  }
  return page;
}

async function navigate(raw) {
  const p = await ensurePage();
  let url = String(raw || "").trim();
  if (!url) return;
  if (!/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(url)) {
    if (url.indexOf(".") >= 0 && url.indexOf(" ") < 0) url = "https://" + url;
    else url = "https://www.google.com/search?q=" + encodeURIComponent(url);
  }
  lastMessage = "Abrindo " + url;
  try {
    await p.goto(url, { waitUntil: "domcontentloaded", timeout: 35000 });
    lastMessage = "Pronto";
  } catch (e) {
    lastMessage = "Falha ao abrir: " + String(e.message || e);
  }
}

const app = express();
app.use(express.json({ limit: "1mb" }));
app.use(express.urlencoded({ extended: false, limit: "1mb" }));

app.get("/", async function(_req, res) {
  await ensurePage();
  res.sendFile(path.join(__dirname, "index.html"));
});
app.get("/health", function(_req, res) {
  res.json({ ok: true, version: "7.1.0" });
});
app.get("/shot", async function(_req, res) {
  try {
    const p = await ensurePage();
    const png = await p.screenshot({ type: "png" });
    res.set("Cache-Control", "no-store, no-cache, must-revalidate");
    res.type("png").send(png);
  } catch (e) {
    res.status(500).send(String(e.message || e));
  }
});
app.get("/api/state", async function(_req, res) {
  const p = await ensurePage();
  let title = "";
  try { title = await p.title(); } catch (_) {}
  res.json({ url: p.url(), title: title, message: lastMessage, download: lastDownload });
});
app.post("/api/nav", async function(req, res) {
  await navigate(req.body && req.body.url);
  res.json({ ok: true });
});
app.post("/api/back", async function(_req, res) {
  const p = await ensurePage();
  try { await p.goBack({ waitUntil: "domcontentloaded", timeout: 20000 }); } catch (_) {}
  res.json({ ok: true });
});
app.post("/api/forward", async function(_req, res) {
  const p = await ensurePage();
  try { await p.goForward({ waitUntil: "domcontentloaded", timeout: 20000 }); } catch (_) {}
  res.json({ ok: true });
});
app.post("/api/reload", async function(_req, res) {
  const p = await ensurePage();
  try { await p.reload({ waitUntil: "domcontentloaded", timeout: 20000 }); } catch (_) {}
  res.json({ ok: true });
});
app.post("/api/click", async function(req, res) {
  const p = await ensurePage();
  const x = Math.max(0, Math.min(1279, Number(req.body.x || 0)));
  const y = Math.max(0, Math.min(719, Number(req.body.y || 0)));
  try { await p.mouse.click(x, y); } catch (_) {}
  res.json({ ok: true });
});
app.post("/api/scroll", async function(req, res) {
  const p = await ensurePage();
  try { await p.mouse.wheel(0, Number(req.body.y || 0)); } catch (_) {}
  res.json({ ok: true });
});
app.post("/api/type", async function(req, res) {
  const p = await ensurePage();
  try { await p.keyboard.insertText(String((req.body && req.body.text) || "")); } catch (_) {}
  res.json({ ok: true });
});
app.post("/api/image-at", async function(req, res) {
  const p = await ensurePage();
  const x = Math.max(0, Math.min(1279, Number(req.body.x || 0)));
  const y = Math.max(0, Math.min(719, Number(req.body.y || 0)));
  try {
    const info = await p.evaluate(function(pos) {
      let el = document.elementFromPoint(pos.x, pos.y);
      for (let i = 0; el && i < 6; i++, el = el.parentElement) {
        if (el.tagName === "IMG") return { url: el.currentSrc || el.src, name: el.alt || "" };
        const bg = getComputedStyle(el).backgroundImage || "";
        const m = bg.match(/^url\(["']?(.*?)["']?\)$/);
        if (m) return { url: m[1], name: "" };
      }
      return null;
    }, { x: x, y: y });
    if (!info || !info.url) {
      lastMessage = "Nenhuma imagem encontrada nesse ponto.";
      return res.json({ ok: false });
    }
    let name = guessName(info.url, "image.jpg");
    if (info.name && name.indexOf(".") < 0) name = safeName(info.name) + ".jpg";
    registerRemote(info.url, name, recentRequests.get(info.url) || null, "image/*");
    res.json({ ok: true });
  } catch (e) {
    lastMessage = "Falha ao identificar imagem: " + String(e.message || e);
    res.json({ ok: false });
  }
});

app.get("/handoff/:kind/:token/:name", function(req, res) {
  if (!downloads.has(req.params.token)) return res.status(404).send("Download expirado.");
  res.type("html").send("<h1>Download capturado pelo PS4.</h1>");
});

app.get("/file/:kind/:token/:name", async function(req, res) {
  const meta = downloads.get(req.params.token);
  if (!meta) return res.status(404).send("Download expirado.");

  if (meta.localPath) {
    if (!meta.ready) return res.status(425).send("Arquivo ainda sendo preparado.");
    res.set("Content-Disposition", "attachment; filename=\"" + meta.filename.replace(/"/g, "") + "\"");
    return res.sendFile(meta.localPath);
  }

  try {
    const cookieList = context ? await context.cookies(meta.url) : [];
    const cookie = cookieList.map(function(c){ return c.name + "=" + c.value; }).join("; ");
    const srcHeaders = (meta.requestInfo && meta.requestInfo.headers) || {};
    const headers = {};
    Object.keys(srcHeaders).forEach(function(k) {
      const lk = k.toLowerCase();
      if (["host","content-length","connection","accept-encoding","range"].indexOf(lk) >= 0) return;
      headers[k] = srcHeaders[k];
    });
    if (cookie) headers.cookie = cookie;
    if (meta.referer) headers.referer = meta.referer;
    if (req.headers.range) headers.range = req.headers.range;
    headers["accept-encoding"] = "identity";

    const method = (meta.requestInfo && meta.requestInfo.method) || "GET";
    const body = method !== "GET" && method !== "HEAD" && meta.requestInfo
      ? (meta.requestInfo.postData || undefined) : undefined;

    const upstream = await fetch(meta.url, { method: method, headers: headers, body: body, redirect: "follow" });
    res.status(upstream.status);
    ["content-type","content-length","content-range","accept-ranges","last-modified","etag"].forEach(function(h) {
      const v = upstream.headers.get(h);
      if (v) res.set(h, v);
    });
    res.set("Content-Disposition", "attachment; filename=\"" + meta.filename.replace(/"/g, "") + "\"");
    if (!upstream.body) return res.end();
    Readable.fromWeb(upstream.body).pipe(res);
  } catch (e) {
    res.status(502).send("Falha no proxy: " + String(e.message || e));
  }
});

const server = app.listen(HTTP_PORT, "0.0.0.0", async function() {
  console.log("[Hybrid] HTTP port:", HTTP_PORT);
  console.log("[Hybrid] LAN hint:", getLanIp());
  try {
    await ensurePage();
  } catch (e) {
    lastMessage = "Chromium nao iniciou: " + String(e.message || e);
    console.error(lastMessage);
  }
});
const udp = dgram.createSocket("udp4");
udp.on("message", function(msg, rinfo) {
  if (String(msg).trim() !== "PBDL_DISCOVER_V1") return;
  const payload = Buffer.from("PBDL_BACKEND_PORT " + HTTP_PORT);
  udp.send(payload, rinfo.port, rinfo.address);
});
udp.bind(DISCOVERY_PORT, "0.0.0.0", function() {
  console.log("[Hybrid] Discovery UDP: " + DISCOVERY_PORT);
});
process.on("SIGINT", async function() {
  try { if (browser) await browser.close(); } catch (_) {}
  server.close(function(){ process.exit(0); });
});
