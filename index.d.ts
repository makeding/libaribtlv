export as namespace createTlvDemuxModule;

declare function createTlvDemuxModule(
  moduleOverrides?: createTlvDemuxModule.TlvDemuxModuleOverrides,
): Promise<createTlvDemuxModule.TlvDemuxModule>;

declare namespace createTlvDemuxModule {
  type TrackKind = "video" | "audio" | "subtitle";
  type Codec = "hevc" | "aac-latm" | "ttml";
  type ErrorCode =
    | "malformed-input"
    | "unsupported-feature"
    | "discontinuity"
    | "resource-limit";
  type IndexState =
    | "absent"
    | "loading"
    | "building"
    | "partial"
    | "following"
    | "complete"
    | "stale"
    | "failed";
  type DurationProbeState =
    | "idle"
    | "need-range"
    | "complete"
    | "unknown"
    | "failed"
    | "cancelled";
  type DurationProbeFailure =
    | "none"
    | "invalid-source"
    | "invalid-response"
    | "source-error"
    | "no-video"
    | "no-tail-timestamp"
    | "range-limit"
    | "parse-error";
  type ApplicationCollectionState = "discovered" | "collecting" | "ready";

  interface TlvDemuxModuleOverrides {
    print?: (text: string) => void;
    printErr?: (text: string) => void;
    onAbort?: (reason: unknown) => void;
    [name: string]: unknown;
  }

  interface Deletable {
    delete(): void;
    isDeleted(): boolean;
  }

  interface DurationInfo {
    value: bigint;
    timescale: number;
    status: "provisional" | "complete";
  }

  interface SeekPoint {
    presentationTimeUs: bigint;
    signallingOffset: bigint;
    randomAccessOffset: bigint;
    videoTrackId: bigint;
    bootstrapId: bigint;
  }

  interface SeekPointPair {
    first: SeekPoint;
    second: SeekPoint | null;
  }

  interface BroadcastClock {
    mediaTimeValue: bigint;
    mediaTimeTimescale: number;
    broadcastTimeValue: bigint;
    broadcastTimeTimescale: number;
    inputOffset: bigint;
    discontinuity: boolean;
  }

  interface ServiceInfo {
    contextId: number;
    packageId: Uint8Array;
  }

  interface AudioTrackInfo {
    componentType: number;
    componentTag: number;
    channelLayout: number;
    /** Actual speaker-channel count; zero when the signalled layout is unknown. */
    channels: number;
    streamType: number;
    simulcastGroupTag: number;
    multilingual: boolean;
    sampleRate: number;
    mainComponent: boolean;
    secondaryLanguage: string;
  }

  interface SubtitleTrackInfo {
    operationMode: number;
    timingMode: number;
  }

  interface TrackInfo {
    trackId: bigint;
    contextId: number;
    packetId: number;
    kind: TrackKind;
    codec: Codec;
    language: string;
    componentTag: number;
    timescale: number;
    audio?: AudioTrackInfo;
    subtitle?: SubtitleTrackInfo;
  }

  interface SubtitleResource {
    subsampleNumber: number;
    dataType: number;
    data: Uint8Array;
  }

  interface AccessUnit {
    trackId: bigint;
    codec: Codec;
    data: Uint8Array;
    ptsValue: bigint;
    ptsTimescale: number;
    dtsValue: bigint;
    dtsTimescale: number;
    mpuSequenceNumber: number | null;
    subtitleReferenceStartPtsValue: bigint | null;
    subtitleReferenceStartPtsTimescale: number | null;
    subtitleResources: SubtitleResource[];
    restartOffset: bigint;
    inputOffset: bigint;
    randomAccess: boolean;
    discontinuity: boolean;
    dataLifetime?: "callback";
  }

  interface DemuxError {
    code: ErrorCode;
    inputOffset: bigint;
    recoverable: boolean;
    message: string;
  }

  interface EventInfo {
    contextId: number;
    sourcePacketId: number;
    tableId: number;
    version: number;
    currentNext: boolean;
    sectionNumber: number;
    lastSectionNumber: number;
    serviceId: number;
    tlvStreamId: number;
    originalNetworkId: number;
    eventId: number;
    startTimeUnixMilliseconds: number | null;
    durationSeconds: number | null;
    runningStatus: number;
    freeCaMode: boolean;
    language: string;
    title: string;
    description: string;
    inputOffset: bigint;
  }

  interface ApplicationState {
    contextId: number;
    sourcePacketId: number;
    applicationType: number;
    organizationId: number;
    applicationId: number;
    controlCode: number;
    version: number;
    entryPath: string;
    transportUrls: string[];
    state: ApplicationCollectionState;
    entryReady: boolean;
    resourceCount: number;
  }

  interface ApplicationResourceMetadata {
    contextId: number;
    componentTag: number;
    transactionId: number;
    downloadId: number;
    mpuSequenceNumber: number;
    itemId: number;
    version: number;
    path: string;
    contentType: string;
    size: number;
    generation: bigint;
  }

  interface ApplicationResource
    extends Omit<ApplicationResourceMetadata, "size" | "generation"> {
    data: Uint8Array;
    dataLifetime?: "callback";
  }

  interface MseTrackInit {
    type: "video" | "audio";
    mime: string;
    data: Uint8Array;
    width: number;
    height: number;
    sampleRate: number;
    channels: number;
  }

  interface MseMediaSegment {
    type: "video" | "audio";
    data: Uint8Array;
  }

  interface MseVideoStart {
    nalType: number;
    signalledRandomAccess: boolean;
  }

  interface TlvDemuxCallbacks {
    onService?: (service: ServiceInfo) => void;
    onTrack?: (track: TrackInfo) => void;
    onAccessUnit?: (unit: AccessUnit) => void;
    onAccessUnitView?: (unit: AccessUnit) => void;
    onError?: (error: DemuxError) => void;
    onBroadcastClock?: (clock: BroadcastClock) => void;
    onEventInfo?: (event: EventInfo) => void;
    onApplicationState?: (application: ApplicationState) => void;
    onApplicationResource?: (resource: ApplicationResource) => void;
    onApplicationResourceView?: (resource: ApplicationResource) => void;
    onApplicationResourcesReset?: () => void;
    onMseInit?: (init: MseTrackInit) => void;
    onMseSegment?: (segment: MseMediaSegment) => void;
    onMseVideoStart?: (start: MseVideoStart) => void;
  }

  interface TlvDemuxOptions extends TlvDemuxCallbacks {
    /** Suppress MSE AAC output above this channel count. Zero or omitted is unlimited. */
    mseMaxAudioChannels?: number;
  }

  interface TlvDemuxer extends Deletable {
    push(bytes: ArrayBufferView): boolean;
    pushFromHeap(address: number, size: number): boolean;
    flush(): void;
    reset(): void;
    reposition(inputOffset: bigint, preserveTimeline: boolean): void;
    selectService(contextId?: number | null): void;
    selectTrack(kind: TrackKind, trackId?: bigint | null): void;
    setMseOutputEnabled(enabled: boolean): void;
    drainApplicationResources(maxEvents: number): boolean;
    startIndex(growing: boolean): void;
    finalizeIndex(): boolean;
    indexState(): IndexState;
    indexDuration(): DurationInfo | null;
    setIndexDuration(durationUs: bigint): boolean;
    seekPointCount(): number;
    indexedVideoTrack(): bigint | null;
    previousSync(targetUs: bigint): SeekPoint | null;
    seekPointsFor(targetUs: bigint): SeekPointPair | null;
    estimateOffset(targetUs: bigint, sourceSize: bigint): bigint | null;
    applicationResources(contextId?: number | null): ApplicationResourceMetadata[];
    applicationResource(contextId: number, path: string): ApplicationResource | null;
    applicationEntry(contextId: number): string | null;
    applications(): ApplicationState[];
    applicationResourceGeneration(): bigint;
    broadcastClock(): BroadcastClock | null;
  }

  interface DurationProbeOptions {
    initialRangeSize?: bigint;
    maxRangeSize?: bigint;
    serviceContextId?: number;
    videoPacketId?: number;
  }

  interface RangeRequest {
    generation: bigint;
    requestId: bigint;
    offset: bigint;
    length: bigint;
  }

  interface DurationProbe extends Deletable {
    begin(sourceSize: bigint, options?: DurationProbeOptions): boolean;
    nextRange(): RangeRequest | null;
    pushRange(
      requestId: bigint,
      absoluteOffset: bigint,
      bytes: ArrayBufferView,
      endOfRange: boolean,
    ): boolean;
    pushRangeFromHeap(
      requestId: bigint,
      absoluteOffset: bigint,
      address: number,
      size: number,
      endOfRange: boolean,
    ): boolean;
    failRange(requestId: bigint): boolean;
    cancel(): void;
    state(): DurationProbeState;
    failure(): DurationProbeFailure;
    duration(): DurationInfo | null;
    generation(): bigint;
    transferredBytes(): bigint;
  }

  interface TlvDemuxModule {
    HEAPU8: Uint8Array;
    _malloc(size: number): number;
    _free(address: number): void;
    TlvDemuxer: new (options: TlvDemuxOptions) => TlvDemuxer;
    DurationProbe: new () => DurationProbe;
  }
}

export = createTlvDemuxModule;
