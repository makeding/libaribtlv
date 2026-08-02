export interface MseAppendQueueOptions {
  retryDelayMilliseconds?: number;
  backBufferSeconds?: number;
  forwardBufferHighSeconds?: number;
  trimGranularitySeconds?: number;
  getMediaError?: (media: HTMLMediaElement) => string;
}

export interface MseBufferedRange {
  start: number;
  end: number;
}

export interface FinalizeMseMediaSourceOptions {
  truncateToCommonEnd?: boolean;
  minimumTruncationSeconds?: number;
}

export declare class MseAppendQueue {
  readonly mediaElement: HTMLMediaElement;
  readonly mediaSource: MediaSource;
  readonly mime: string;
  readonly sourceBuffer: SourceBuffer;
  readonly queue: Uint8Array[];
  queuedBytes: number;
  currentBytes: number;
  readonly waiters: unknown[];
  error: Error | null;
  retryTimer: ReturnType<typeof setTimeout> | null;
  trimBeforeTime: number | null;
  forceTrim: boolean;
  state: 'running' | 'quiescing' | 'idle' | 'destroyed';
  onUpdateEnd: (() => void) | null;

  constructor(
    mediaSource: MediaSource,
    mediaElement: HTMLMediaElement,
    mime: string,
    onUpdateEnd?: (() => void) | null,
    options?: MseAppendQueueOptions,
  );

  append(data: Uint8Array): void;
  pump(): void;
  bufferedAhead(): number;
  bufferedRanges(): MseBufferedRange[];
  trimBefore(time: number, force?: boolean): void;
  waitBelow(limit: number): Promise<void>;
  isIdle(): boolean;
  waitIdle(): Promise<void>;
  quiesce(): Promise<void>;
  resume(): void;
  stop(): void;
  destroy(error?: Error): void;
}

export declare function finalizeMseMediaSource(
  mediaSource: MediaSource,
  queues: MseAppendQueue[],
  options?: FinalizeMseMediaSourceOptions,
): Promise<{truncatedTo: number | null}>;

export default MseAppendQueue;
