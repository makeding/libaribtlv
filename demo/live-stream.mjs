function joinChunks(chunks, byteLength) {
  if (chunks.length === 1) return chunks[0];
  const output = new Uint8Array(byteLength);
  let offset = 0;
  for (const chunk of chunks) {
    output.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return output;
}

export async function* coalesceReadableStream(reader, {
  targetBytes = 512 * 1024,
  maxDelayMilliseconds = 25,
  now = () => performance.now(),
} = {}) {
  if (!Number.isSafeInteger(targetBytes) || targetBytes <= 0) {
    throw new TypeError('targetBytes must be a positive safe integer');
  }
  if (!Number.isFinite(maxDelayMilliseconds) || maxDelayMilliseconds < 0) {
    throw new TypeError('maxDelayMilliseconds must be a non-negative number');
  }

  let pendingRead = reader.read();
  let chunks = [];
  let chunkBytes = 0;
  let flushDeadline = 0;

  const flush = () => {
    const output = joinChunks(chunks, chunkBytes);
    chunks = [];
    chunkBytes = 0;
    flushDeadline = 0;
    return output;
  };

  try {
    while (true) {
      let result;
      if (chunkBytes === 0) {
        result = { kind: 'read', value: await pendingRead };
      } else {
        const remaining = Math.max(0, flushDeadline - now());
        result = await Promise.race([
          pendingRead.then(value => ({ kind: 'read', value })),
          new Promise(resolve => setTimeout(
            () => resolve({ kind: 'deadline' }), remaining,
          )),
        ]);
      }
      if (result.kind === 'deadline') {
        yield flush();
        continue;
      }

      const { done, value } = result.value;
      if (done) {
        if (chunkBytes !== 0) yield flush();
        break;
      }
      pendingRead = reader.read();
      if (!value?.byteLength) continue;
      if (chunkBytes === 0 && value.byteLength >= targetBytes) {
        yield value;
        continue;
      }
      if (chunkBytes === 0) flushDeadline = now() + maxDelayMilliseconds;
      chunks.push(value);
      chunkBytes += value.byteLength;
      if (chunkBytes >= targetBytes) yield flush();
    }
  } finally {
    await reader.cancel().catch(() => {});
    reader.releaseLock();
  }
}
