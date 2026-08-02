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
  type ApplicationLifecycleState =
    | "unsupported"
    | "autostart-pending"
    | "autostart-ready"
    | "present"
    | "prefetching"
    | "prefetched"
    | "killed";

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

  interface MpuPresentationRegion {
    mpuSequenceNumber: number;
    layoutNumber: number;
    regionNumber: number;
  }

  interface LayoutRegion {
    regionNumber: number;
    leftTopPosX: number;
    leftTopPosY: number;
    rightDownPosX: number;
    rightDownPosY: number;
    layerOrder: number;
  }

  interface LayoutDevice {
    layoutNumber: number;
    deviceId: number;
    regions: LayoutRegion[];
  }

  interface LayoutConfiguration {
    contextId: number;
    sourcePacketId: number;
    version: number;
    devices: LayoutDevice[];
    /** ARIB STD-B60 RGB value encoded as 0xRRGGBB, or null when absent. */
    backgroundColorRgb: number | null;
    inputOffset: bigint;
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
    presentationRegions: MpuPresentationRegion[];
    audio?: AudioTrackInfo;
    subtitle?: SubtitleTrackInfo;
  }

  interface ApplicationServiceInfo {
    contextId: number;
    applicationFormat: number;
    documentResolution: number;
    defaultAit: boolean;
    hasDataTransmissionMessages: boolean;
    aitPacketId: number | null;
    dataTransmissionPacketId: number | null;
  }

  interface DataAssetInfo {
    contextId: number;
    packetId: number;
    assetType: string;
    componentTag: number;
    presentationRegions: MpuPresentationRegion[];
  }

  interface ApplicationInfo {
    contextId: number;
    sourcePacketId: number;
    applicationType: number;
    organizationId: number;
    applicationId: number;
    controlCode: number;
    version: number;
    currentNext: boolean;
    sectionNumber: number;
    lastSectionNumber: number;
    presentApplicationPriority: boolean;
    applicationPriority: number;
    entryPath: string;
    inputOffset: bigint;
  }

  interface MptSnapshot {
    contextId: number;
    sourcePacketId: number;
    packageId: Uint8Array;
    version: number;
    mode: number;
    inputOffset: bigint;
    tracks: TrackInfo[];
    applicationServices: ApplicationServiceInfo[];
    dataAssets: DataAssetInfo[];
  }

  interface MhAitSnapshot {
    contextId: number;
    sourcePacketId: number;
    applicationType: number;
    version: number;
    currentNext: boolean;
    inputOffset: bigint;
    applications: ApplicationInfo[];
  }

  interface ServiceStateReset {
    contextId: number | null;
    reason: "full-reset" | "service-selection";
  }

  interface SubtitleResource {
    subsampleNumber: number;
    dataType: number;
    data: Uint8Array;
  }

  interface AccessUnit {
    trackId: bigint;
    codec: Codec;
    componentTag: number;
    subtitleTimingMode: number | null;
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
    extendedDescription: string;
    extendedItems: Array<{ description: string; value: string }>;
    genres: Array<{ level1: number; level2: number; user1: number; user2: number }>;
    parentalRatings: Array<{ countryCode: string; rating: number }>;
    audioComponents: Array<{
      componentType: number;
      componentTag: number;
      channelLayout: number;
      channels: number;
      streamType: number;
      multilingual: boolean;
      mainComponent: boolean;
      sampleRate: number;
      language: string;
      secondaryLanguage: string;
      text: string;
    }>;
    series: {
      seriesId: number;
      repeatLabel: number;
      programPattern: number;
      expireDateMjd: number | null;
      episodeNumber: number;
      lastEpisodeNumber: number;
      name: string;
    } | null;
    inputOffset: bigint;
  }

  interface ServiceDescriptionInfo {
    serviceId: number;
    eitUserDefinedFlags: number;
    eitSchedule: boolean;
    eitPresentFollowing: boolean;
    runningStatus: number;
    freeCaMode: boolean;
    serviceType: number;
    providerName: string;
    serviceName: string;
  }

  interface MhSdtSnapshot {
    contextId: number;
    sourcePacketId: number;
    tableId: number;
    tlvStreamId: number;
    originalNetworkId: number;
    version: number;
    currentNext: boolean;
    services: ServiceDescriptionInfo[];
    inputOffset: bigint;
  }

  interface LocalTimeOffsetInfo {
    countryCode: string;
    countryRegionId: number;
    polarity: boolean;
    offsetMinutes: number;
    changeTimeUnixMilliseconds: number | null;
    nextOffsetMinutes: number;
  }

  interface MhTotInfo {
    contextId: number;
    sourcePacketId: number;
    timeUnixMilliseconds: number;
    localTimeOffsets: LocalTimeOffsetInfo[];
    inputOffset: bigint;
  }

  interface StreamEvent {
    contextId: number;
    sourcePacketId: number;
    eventMessageTag: number;
    dataEventId: number;
    messageGroupId: number;
    messageVersion: number;
    currentNext: boolean;
    sectionNumber: number;
    lastSectionNumber: number;
    timeMode: number;
    timeValue: bigint;
    utcReference: bigint | null;
    nptReference: bigint | null;
    messageType: number;
    /** Raw 16-bit ARIB STD-B60 event_msg_id, retained for diagnostics. */
    rawMessageId: number;
    /** High octet exposed as message_id to ARIB STD-B62 applications. */
    messageId: number;
    privateData: Uint8Array;
    inputOffset: bigint;
  }

  /**
   * Receiver-level TR-B39 viewer-participation corner notification. This is
   * intentionally separate from StreamEvent because it is not dispatched to
   * the ARIB-HTML5 application as an interrupt event.
   */
  interface ViewerParticipationNotification {
    contextId: number;
    sourcePacketId: number;
    eventMessageTag: number;
    dataEventId: number;
    messageGroupId: number;
    version: number;
    currentNext: boolean;
    sectionNumber: number;
    lastSectionNumber: number;
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
    currentNext: boolean;
    sectionNumber: number;
    lastSectionNumber: number;
    inputOffset: bigint;
    applicationDescriptorPresent: boolean;
    profiles: Array<{
      applicationProfile: number;
      versionMajor: number;
      versionMinor: number;
      versionMicro: number;
    }>;
    serviceBound: boolean;
    /** MH-application descriptor visibility: 0/1 hidden from users, 3 visible. */
    visibility: number;
    presentApplicationPriority: boolean;
    applicationPriority: number;
    transportProtocolLabels: number[];
    transports: Array<{ protocolId: number; label: number; urls: string[] }>;
    entryPath: string;
    transportUrls: string[];
    state: ApplicationCollectionState;
    /** Broadcast-requested lifecycle; this does not claim the HTML runtime started. */
    lifecycle: ApplicationLifecycleState;
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

  interface ApplicationResourceRemoval {
    contextId: number;
    componentTag: number;
    transactionId: number;
    downloadId: number;
    mpuSequenceNumber: number;
    itemId: number;
    path: string;
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
    onTrackRemoved?: (track: TrackInfo) => void;
    onApplicationServiceRemoved?: (service: ApplicationServiceInfo) => void;
    onDataAssetRemoved?: (asset: DataAssetInfo) => void;
    onApplicationRemoved?: (application: ApplicationInfo) => void;
    onMptSnapshot?: (snapshot: MptSnapshot) => void;
    onMhAitSnapshot?: (snapshot: MhAitSnapshot) => void;
    onServiceStateReset?: (reset: ServiceStateReset) => void;
    onLayoutConfiguration?: (layout: LayoutConfiguration) => void;
    onAccessUnit?: (unit: AccessUnit) => void;
    onAccessUnitView?: (unit: AccessUnit) => void;
    /**
     * Callback-lifetime TTML payloads plus payload-free HEVC RAP/discontinuity
     * and AAC discontinuity events, intended for browser playback control.
     */
    onPlaybackAccessUnitView?: (unit: AccessUnit) => void;
    onError?: (error: DemuxError) => void;
    onBroadcastClock?: (clock: BroadcastClock) => void;
    onEventInfo?: (event: EventInfo) => void;
    onMhSdtSnapshot?: (snapshot: MhSdtSnapshot) => void;
    onMhTot?: (info: MhTotInfo) => void;
    onStreamEvent?: (event: StreamEvent) => void;
    onViewerParticipationNotification?: (
      notification: ViewerParticipationNotification,
    ) => void;
    onApplicationState?: (application: ApplicationState) => void;
    onApplicationResource?: (resource: ApplicationResource) => void;
    onApplicationResourceView?: (resource: ApplicationResource) => void;
    onApplicationResourceRemoved?: (removal: ApplicationResourceRemoval) => void;
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
    setSubtitlePassthroughEnabled(enabled: boolean): void;
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
