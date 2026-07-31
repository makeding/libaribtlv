import { createReadStream } from 'node:fs';
import { stat } from 'node:fs/promises';
import { createServer } from 'node:http';
import { extname, resolve, sep } from 'node:path';

const root = resolve(process.cwd());
const aribb62Root = resolve(root, '..', 'aribb62.js');
const aribHtml5Root = resolve(root, '..', 'libaribhtml5', 'dist', 'sdk');
const port = Number(process.argv[2] || 8000);
const mimeTypes = new Map([
  ['.css', 'text/css; charset=utf-8'],
  ['.html', 'text/html; charset=utf-8'],
  ['.js', 'text/javascript; charset=utf-8'],
  ['.mjs', 'text/javascript; charset=utf-8'],
  ['.wasm', 'application/wasm'],
  ['.mmts', 'application/octet-stream'],
  ['.tlv', 'application/octet-stream'],
]);

function parseRange(value, size) {
  const match = /^bytes=(\d*)-(\d*)$/.exec(value || '');
  if (!match || (!match[1] && !match[2])) return null;
  let start;
  let end;
  if (!match[1]) {
    const suffix = Number(match[2]);
    if (!Number.isSafeInteger(suffix) || suffix <= 0) return null;
    start = Math.max(0, size - suffix);
    end = size - 1;
  } else {
    start = Number(match[1]);
    end = match[2] ? Number(match[2]) : size - 1;
  }
  if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) ||
      start < 0 || start >= size || end < start) return null;
  return { start, end: Math.min(end, size - 1) };
}

const server = createServer(async (request, response) => {
  try {
    if (request.method !== 'GET' && request.method !== 'HEAD') {
      response.writeHead(405, { Allow: 'GET, HEAD' });
      response.end();
      return;
    }
    const url = new URL(request.url || '/', 'http://localhost');
    let pathname = decodeURIComponent(url.pathname);
    let staticRoot = root;
    if (pathname.startsWith('/aribb62.js/')) {
      staticRoot = aribb62Root;
      pathname = pathname.slice('/aribb62.js'.length);
    } else if (pathname.startsWith('/libaribhtml5/')) {
      staticRoot = aribHtml5Root;
      pathname = pathname.slice('/libaribhtml5'.length);
    }
    let filename = resolve(staticRoot, `.${pathname}`);
    if (filename !== staticRoot && !filename.startsWith(`${staticRoot}${sep}`)) {
      response.writeHead(403);
      response.end('Forbidden');
      return;
    }
    let info = await stat(filename);
    if (info.isDirectory()) {
      filename = resolve(filename, 'index.html');
      info = await stat(filename);
    }
    if (!info.isFile()) throw Object.assign(new Error('Not found'), { code: 'ENOENT' });

    const headers = {
      'Accept-Ranges': 'bytes',
      'Cache-Control': 'no-cache',
      'Content-Type': mimeTypes.get(extname(filename).toLowerCase()) || 'application/octet-stream',
    };
    if (filename === resolve(aribHtml5Root, 'arib-vfs-sw.js')) {
      headers['Service-Worker-Allowed'] = '/data-broadcast/';
    }
    const requestedRange = request.headers.range;
    if (requestedRange) {
      const range = parseRange(requestedRange, info.size);
      if (!range) {
        response.writeHead(416, { ...headers, 'Content-Range': `bytes */${info.size}` });
        response.end();
        return;
      }
      const length = range.end - range.start + 1;
      response.writeHead(206, {
        ...headers,
        'Content-Length': length,
        'Content-Range': `bytes ${range.start}-${range.end}/${info.size}`,
      });
      if (request.method === 'HEAD') response.end();
      else createReadStream(filename, { start: range.start, end: range.end }).pipe(response);
      return;
    }

    response.writeHead(200, { ...headers, 'Content-Length': info.size });
    if (request.method === 'HEAD') response.end();
    else createReadStream(filename).pipe(response);
  } catch (error) {
    const status = error?.code === 'ENOENT' ? 404 : 500;
    response.writeHead(status, { 'Content-Type': 'text/plain; charset=utf-8' });
    response.end(status === 404 ? 'Not found' : String(error));
  }
});

server.listen(port, '127.0.0.1', () => {
  console.log(`tlvdemux demo: http://127.0.0.1:${port}/demo/`);
  console.log(`serving ${root} with HTTP Range support`);
});
