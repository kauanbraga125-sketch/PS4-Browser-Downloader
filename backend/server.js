"use strict";
const express = require("express");
const { chromium } = require("playwright");
const dgram = require("dgram");
const net = require("net");
const os = require("os");
const crypto = require("crypto");
const path = require("path");
const fs = require("fs");
const { Readable } = require("stream");

const HTTP_PORT = Number(process.env.PBDL_HTTP_PORT || 32124);
const DISCOVERY_PORT = Number(process.env.PBDL_DISCOVERY_PORT || 32123);
const FRAME_PORT = Number(process.env.PBDL_FRAME_PORT || (HTTP_PORT + 1));
const VIEWPORT = { width: 1280, height: 720 };
const STREAM_WIDTH = Number(process.env.PBDL_STREAM_WIDTH || 854);
const STREAM_HEIGHT = Number(process.env.PBDL_STREAM_HEIGHT || 480);
const STREAM_QUALITY = Number(process.env.PBDL_STREAM_QUALITY || 32);

let browser = null;
let context = null;
let page = null;
let cdpSession = null;
let latestFrame = null;
let latestFrameSeq = 0;
let latestFrameAt = 0;
let lastDownload = null;
let lastMessage = "Inicializando Chromium...";
const downloads = new Map();
const recentRequests = new Map();
const frameClients = new Set();

function sendFrameToClient(socket, frame, seq) {
  if (!frame || !socket || socket.destroyed) return;
  // Never queue many old frames. A remote browser should display the newest
  // page state, not faithfully replay stale frames.
  if (socket.writableLength > 512 * 1024) return;

  const header = Buffer.allocUnsafe(12);
  header.write("PBDL", 0, 4, "ascii");
  header.writeUInt32BE((seq >>> 0), 4);
  header.writeUInt32BE(frame.length >>> 0, 8);
  socket.cork();
  socket.write(header);
  socket.write(frame);
  socket.uncork();
}

function broadcastFrame(frame, seq) {
  for (const socket of frameClients)
    sendFrameToClient(socket, frame, seq);
}

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

async function startCdpScreencast(p) {
  try {
    if (cdpSession) {
      try { await cdpSession.detach(); } catch (_) {}
      cdpSession = null;
    }
    cdpSession = await context.newCDPSession(p);
    cdpSession.on("Page.screencastFrame", async function(evt) {
      latestFrame = Buffer.from(evt.data, "base64");
      latestFrameSeq++;
      latestFrameAt = Date.now();
      broadcastFrame(latestFrame, latestFrameSeq);
      try {
        await cdpSession.send("Page.screencastFrameAck", {
          sessionId: evt.sessionId
        });
      } catch (_) {}
    });
    await cdpSession.send("Page.startScreencast", {
      format: "jpeg",
      quality: STREAM_QUALITY,
      maxWidth: STREAM_WIDTH,
      maxHeight: STREAM_HEIGHT,
      everyNthFrame: 1
    });
    console.log("[Hybrid] CDP screencast ativo");
  } catch (e) {
    cdpSession = null;
    console.error("[Hybrid] CDP screencast indisponivel:", String(e.message || e));
  }
}

async function ensurePage() {
  if (page && !page.isClosed()) return page;

  if (!browser) {
    browser = await chromium.launch({
      headless: true,
      args: [
        "--disable-dev-shm-usage",
        "--disable-background-timer-throttling",
        "--disable-renderer-backgrounding",
        "--disable-backgrounding-occluded-windows",
        "--force-device-scale-factor=1"
      ]
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

  await startCdpScreencast(page);

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


function escHtml(v) {
  return String(v == null ? "" : v)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

async function renderLegacyPage(req, res) {
  const p = await ensurePage();
  let title = "";
  try { title = await p.title(); } catch (_) {}
  const mode = req.query && req.query.mode === "image" ? "image" : "click";
  const shotAction = mode === "image" ? "/legacy/image-at" : "/legacy/click";
  const shotLabel = mode === "image"
    ? "MODO BAIXAR IMAGEM: clique na imagem desejada"
    : "MODO NAVEGAR: clique na pagina abaixo";

  let dl = "";
  if (lastDownload) {
    if (lastDownload.preparing) {
      dl = "<p><b>Download:</b> Chromium baixando no PC: " +
        escHtml(lastDownload.filename) + "</p>";
    } else if (lastDownload.handoff) {
      dl = "<p><b>Download pronto:</b> " + escHtml(lastDownload.filename) +
        " &nbsp; <a href=\"" + escHtml(lastDownload.handoff) +
        "\"><b>BAIXAR NO PS4</b></a></p>";
    }
  }

  const html =
    "<!DOCTYPE html><html><head>" +
    "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">" +
    "<title>PS4 Hybrid Browser</title></head>" +
    "<body bgcolor=\"#FFFFFF\" text=\"#000000\" link=\"#0000CC\" vlink=\"#660099\">" +
    "<h2>PS4 Hybrid Browser v7.2</h2>" +
    "<table border=\"0\" cellpadding=\"4\" cellspacing=\"0\" width=\"100%\"><tr>" +
    "<td><form method=\"post\" action=\"/legacy/back\"><input type=\"submit\" value=\"&lt;- Voltar\"></form></td>" +
    "<td><form method=\"post\" action=\"/legacy/forward\"><input type=\"submit\" value=\"Avancar -&gt;\"></form></td>" +
    "<td><form method=\"post\" action=\"/legacy/reload\"><input type=\"submit\" value=\"Recarregar\"></form></td>" +
    "<td><form method=\"post\" action=\"/legacy/scroll-up\"><input type=\"submit\" value=\"Subir\"></form></td>" +
    "<td><form method=\"post\" action=\"/legacy/scroll-down\"><input type=\"submit\" value=\"Descer\"></form></td>" +
    "<td><form method=\"get\" action=\"/ps4\"><input type=\"hidden\" name=\"mode\" value=\"" +
      (mode === "image" ? "click" : "image") + "\"><input type=\"submit\" value=\"" +
      (mode === "image" ? "Voltar ao modo navegar" : "Baixar imagem") + "\"></form></td>" +
    "</tr></table>" +
    "<form method=\"post\" action=\"/legacy/nav\">" +
    "<b>URL ou pesquisa:</b> <input type=\"text\" name=\"url\" size=\"70\" value=\"" +
      escHtml(p.url()) + "\"> <input type=\"submit\" value=\"Ir\">" +
    "</form>" +
    "<form method=\"post\" action=\"/legacy/type\">" +
    "<b>Texto para campo focado:</b> <input type=\"text\" name=\"text\" size=\"45\">" +
    " <input type=\"submit\" value=\"Enviar texto\"></form>" +
    "<p><b>Titulo:</b> " + escHtml(title) + "<br>" +
    "<b>Status:</b> " + escHtml(lastMessage) + "<br>" +
    "<b>Modo:</b> " + escHtml(shotLabel) + "</p>" +
    dl +
    "<p><a href=\"/ps4?mode=" + mode + "\">Atualizar tela</a></p>" +
    "<form method=\"post\" action=\"" + shotAction + "\">" +
    "<input type=\"hidden\" name=\"mode\" value=\"" + mode + "\">" +
    "<input type=\"image\" name=\"shot\" src=\"/shot?t=" + Date.now() +
      "\" width=\"1280\" height=\"720\" alt=\"Pagina Chromium\">" +
    "</form>" +
    "</body></html>";

  res.set("Cache-Control", "no-store, no-cache, must-revalidate");
  res.type("html").send(html);
}

app.get("/", function(req, res) {
  res.redirect(302, "/ps4");
});
app.get("/ps4", function(req, res) {
  renderLegacyPage(req, res).catch(function(e) {
    res.status(500).type("text").send("Hybrid UI error: " + String(e.message || e));
  });
});
app.get("/health", function(_req, res) {
  res.json({ ok: true, version: "7.8.0", framePort: FRAME_PORT, stream: STREAM_WIDTH + "x" + STREAM_HEIGHT });
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

app.get("/shot-fast", async function(_req, res) {
  try {
    await ensurePage();
    let jpg = latestFrame;
    if (!jpg) {
      jpg = await page.screenshot({
        type: "jpeg",
        quality: STREAM_QUALITY
      });
      latestFrame = jpg;
      latestFrameSeq++;
      latestFrameAt = Date.now();
    }
    res.set("Cache-Control", "no-store, no-cache, must-revalidate");
    res.set("X-Frame-Seq", String(latestFrameSeq));
    res.set("X-Frame-Age", String(Date.now() - latestFrameAt));
    res.type("jpeg").send(jpg);
  } catch (e) {
    res.status(500).send(String(e.message || e));
  }
});


app.get("/api/pending-download", function(_req, res) {
  if (!lastDownload || lastDownload.preparing || !lastDownload.handoff)
    return res.status(204).end();
  res.set("Cache-Control", "no-store");
  res.type("text").send(lastDownload.handoff);
});

app.post("/api/ack-download", function(_req, res) {
  lastDownload = null;
  res.json({ ok: true });
});

app.get("/api/focus-info", async function(_req, res) {
  const p = await ensurePage();
  let editable = false;
  try {
    editable = await p.evaluate(function() {
      var el = document.activeElement;
      if (!el) return false;
      var tag = String(el.tagName || "").toLowerCase();
      var type = String(el.type || "").toLowerCase();
      if (tag === "textarea") return true;
      if (el.isContentEditable) return true;
      if (tag !== "input") return false;
      return ["button","submit","reset","checkbox","radio","file","image","hidden"].indexOf(type) < 0;
    });
  } catch (_) {}
  res.type("text").send(editable ? "1" : "0");
});

app.post("/api/type-submit", async function(req, res) {
  const p = await ensurePage();
  try {
    await p.keyboard.insertText(String((req.body && req.body.text) || ""));
    await p.keyboard.press("Enter");
  } catch (_) {}
  res.json({ ok: true });
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

app.post("/api/home", async function(_req, res) {
  const p = await ensurePage();
  try {
    await p.goto("https://www.google.com/", {
      waitUntil: "domcontentloaded",
      timeout: 25000
    });
    lastMessage = "Google";
  } catch (e) {
    lastMessage = "Falha voltando ao Google: " + String(e.message || e);
  }
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
  res.type("html").send("<html><body bgcolor=\"#FFFFFF\" text=\"#000000\"><h1>Download capturado.</h1><p>Se esta pagina apareceu, o callback do PKG nao interceptou o handoff.</p></body></html>");
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


const frameServer = net.createServer(function(socket) {
  socket.setNoDelay(true);
  socket.setKeepAlive(true, 1000);
  frameClients.add(socket);

  // A newly connected PS4 should not wait for the next page animation.
  if (latestFrame)
    sendFrameToClient(socket, latestFrame, latestFrameSeq);

  socket.on("error", function(){});
  socket.on("close", function() {
    frameClients.delete(socket);
  });
});
frameServer.listen(FRAME_PORT, "0.0.0.0", function() {
  console.log("[Hybrid] Frame stream TCP:", FRAME_PORT,
              STREAM_WIDTH + "x" + STREAM_HEIGHT,
              "JPEG q=" + STREAM_QUALITY);
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
  for (const socket of frameClients) {
    try { socket.destroy(); } catch (_) {}
  }
  frameServer.close(function(){});
  server.close(function(){ process.exit(0); });
});
