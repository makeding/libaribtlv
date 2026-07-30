const resources = new Map();
const waiters = new Map();
let enabled = false;

const broadcastPath = /^\/(?:sh[48]|[4567][012])\//;

function normalizePath(value) {
  let pathname;
  try {
    pathname = decodeURIComponent(new URL(String(value), self.location.origin).pathname);
  } catch {
    return null;
  }
  const parts = pathname.split('/').filter(Boolean);
  if (!parts.length || parts.some(part => part === '.' || part === '..')) return null;
  return parts.join('/');
}

function contentType(path, supplied) {
  if (supplied) return supplied;
  const extension = path.slice(path.lastIndexOf('.')).toLowerCase();
  return new Map([
    ['.html', 'text/html; charset=utf-8'],
    ['.htm', 'text/html; charset=utf-8'],
    ['.css', 'text/css; charset=utf-8'],
    ['.js', 'text/javascript; charset=utf-8'],
    ['.json', 'application/json; charset=utf-8'],
    ['.svg', 'image/svg+xml'],
    ['.png', 'image/png'],
    ['.jpg', 'image/jpeg'],
    ['.jpeg', 'image/jpeg'],
    ['.gif', 'image/gif'],
    ['.webp', 'image/webp'],
    ['.woff', 'font/woff'],
    ['.woff2', 'font/woff2'],
  ]).get(extension) || 'application/octet-stream';
}

function wake(path, resource) {
  const pending = waiters.get(path);
  if (!pending) return;
  waiters.delete(path);
  for (const resolve of pending) resolve(resource);
}

function waitFor(path, timeout = 10000) {
  const current = resources.get(path);
  if (current) return Promise.resolve(current);
  return new Promise(resolve => {
    const pending = waiters.get(path) || new Set();
    pending.add(resolve);
    waiters.set(path, pending);
    setTimeout(() => {
      pending.delete(resolve);
      if (!pending.size) waiters.delete(path);
      resolve(null);
    }, timeout);
  });
}

function rewriteBroadcastObjects(source) {
  return source.replace(/<object\b[^>]*>/gi, tag => {
    const broadcast = /\btype\s*=\s*(?:["']video\/x-arib2-broadcast["']|video\/x-arib2-broadcast)(?:\s|\/?>)/i
      .test(tag);
    if (!broadcast) return tag;
    // Keep the <object> element so application CSS such as "#vstream object"
    // continues to define the receiver's video plane.  Removing only `type`
    // prevents the browser from trying to instantiate an unavailable plugin.
    return tag
      .replace(/\s+type\s*=\s*(?:["']video\/x-arib2-broadcast["']|video\/x-arib2-broadcast)/i,
        ' data-arib-type="video/x-arib2-broadcast"')
      .replace(/\s+data\s*=\s*(["'])(.*?)\1/i, ' data-arib-data=$1$2$1');
  });
}

function deferRomSounds(source) {
  return source.replace(
    /\s+src\s*=\s*(["'])(romsound:\/\/\d+)\1/gi,
    ' data-arib-romsound=$1$2$1',
  );
}

function injectRuntime(bytes) {
  const decoder = new TextDecoder();
  const encoder = new TextEncoder();
  const source = deferRomSounds(rewriteBroadcastObjects(decoder.decode(bytes)));
  const bootstrap = '<script>parent.__ARIB_HTML5_INSTALL__?.(window)</script>';
  const match = /<head(?:\s[^>]*)?>/i.exec(source);
  if (!match) return encoder.encode(`${bootstrap}${source}`);
  const offset = match.index + match[0].length;
  return encoder.encode(`${source.slice(0, offset)}${bootstrap}${source.slice(offset)}`);
}

async function serve(request) {
  const path = normalizePath(request.url);
  if (!path) return new Response('Bad broadcast path', { status: 400 });
  const resource = await waitFor(path);
  if (!resource) return new Response('Broadcast resource is not available', { status: 404 });

  const type = contentType(path, resource.contentType);
  const body = /^text\/html(?:;|$)/i.test(type)
    ? injectRuntime(resource.data)
    : resource.data.slice(0);
  return new Response(request.method === 'HEAD' ? null : body, {
    status: 200,
    headers: {
      'Cache-Control': 'no-store',
      'Content-Type': type,
      'Content-Security-Policy': "default-src 'self' data: blob:; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; object-src 'none'; frame-src 'none'",
      'X-Content-Type-Options': 'nosniff',
    },
  });
}

self.addEventListener('install', event => {
  self.skipWaiting();
  event.waitUntil(Promise.resolve());
});

self.addEventListener('activate', event => {
  event.waitUntil(self.clients.claim());
});

self.addEventListener('message', event => {
  const message = event.data || {};
  const reply = value => event.ports[0]?.postMessage(value);
  if (message.type === 'arib-vfs-begin') {
    enabled = true;
    resources.clear();
    for (const pending of waiters.values()) {
      for (const resolve of pending) resolve(null);
    }
    waiters.clear();
    reply({ ok: true });
    return;
  }
  if (message.type === 'arib-vfs-put') {
    const path = normalizePath(`/${message.path || ''}`);
    if (!path || !(message.data instanceof ArrayBuffer)) {
      reply({ ok: false, error: 'invalid resource' });
      return;
    }
    const resource = {
      data: new Uint8Array(message.data),
      contentType: String(message.contentType || ''),
    };
    resources.set(path, resource);
    wake(path, resource);
    reply({ ok: true });
    return;
  }
  if (message.type === 'arib-vfs-reset') {
    enabled = false;
    resources.clear();
    for (const pending of waiters.values()) {
      for (const resolve of pending) resolve(null);
    }
    waiters.clear();
    reply({ ok: true });
  }
});

self.addEventListener('fetch', event => {
  const url = new URL(event.request.url);
  if (!enabled || url.origin !== self.location.origin ||
      !broadcastPath.test(url.pathname) ||
      (event.request.method !== 'GET' && event.request.method !== 'HEAD')) return;
  event.respondWith(serve(event.request));
});
