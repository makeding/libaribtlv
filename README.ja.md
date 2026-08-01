# tlvdemux

[English](README.md) | 日本語

`tlvdemux` は、復号済みの ARIB MMT/TLV ストリームを対象とする C++20 製の
インクリメンタル・デマルチプレクサーです。ストリームを MPEG-TS に変換したり、
FFmpeg の ABI 型を公開したりせずに、プレーヤーでそのまま扱える HEVC、
AAC-LATM/LOAS、ARIB STD-B62 TTML のアクセスユニットを出力します。

現在の実装は、安定した公開コールバック API、上限付きのインクリメンタルな
TLV 再同期、圧縮 IP コンテキストの分離、MMTP のフラグメント／アグリゲーション、
PA/M2/MPT によるトラック検出、記述子に基づくタイムライン、および
HEVC/AAC-LATM/TTML アクセスユニット出力を提供します。

## ビルドとテスト

```sh
nix-shell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

標準外のランタイム依存関係は Zlib のみです。圧縮されたデータ放送項目を、
ブラウザーで利用できる仮想ファイルに展開するために使用します。Emscripten では、
単一ファイルの WASM ビルドに zlib port がリンクされます。

共有ライブラリはデフォルトで有効です。Linux では `libtlvdemux.so.0`
（およびバージョン付きの実体ファイル）、macOS では対応する
`libtlvdemux.0.dylib` が生成されます。静的な `libtlvdemux.a` が必要な場合は
`-DBUILD_SHARED_LIBS=OFF` を指定してください。公開インターフェースは C++20 ABI
なので、動的リンクする利用側でも互換性のあるコンパイラーと C++ 標準ライブラリを
使用してください。

`add_subdirectory()` でプロジェクトに組み込む場合、診断用実行ファイルは
`-DTLVDEMUX_BUILD_TOOLS=OFF` で無効化できます。テストは CMake 標準の
`BUILD_TESTING` オプションに従います。

macOS では、ブラウザーを起動せずに VideoToolbox probe でブラウザー向け MSE
経路全体を検証できます。この probe は MMTS を実際の `MseRemuxer` に入力し、
`tfdt`／`trun` の連続性と HEVC サンプルフラグを検証し、Chromium と同様の
`hvc1` 変換を適用してハードウェアデコーダーへ渡します。次の例では、サンプルを
3 倍速で送りながら、決定的なランダムバイト位置からの再生を 16 回繰り返します。

```sh
./build/tlvdemux-videotoolbox-probe demo/8k.mmts \
  --mse --rate 3 --inflight 4 --max-au 90 \
  --random-seeks 16 --seed 20260731
```

ライブラリ、公開ヘッダー、CMake ターゲット定義は次のようにインストールします。

```sh
cmake --install build --prefix /desired/prefix
```

共有または静的ライブラリ、公開ヘッダー、CMake パッケージターゲット
`tlvdemux::tlvdemux`、有効な場合は診断ツール、および MIT ライセンスが
インストールされます。

## ライブラリの使い方

`tlvdemux::Sink` を実装し、demuxer の生存期間中はそのインスタンスを保持して、
任意のサイズに分割したデータを同期的に入力します。

```cpp
#include <tlvdemux/demuxer.hpp>

class PlayerSink final : public tlvdemux::Sink {
public:
    void onService(const tlvdemux::ServiceInfo& service) override;
    void onTrack(const tlvdemux::TrackInfo& track) override;
    void onAccessUnit(tlvdemux::AccessUnit&& unit) override;
    void onError(const tlvdemux::Error& error) override;
};

PlayerSink sink;
tlvdemux::Demuxer demuxer(sink);
demuxer.push(data, size);
demuxer.flush();
```

`push()` は入力ポインターを保持しません。コールバックの payload は自身でデータを
所有します。不正なストリームデータは `onError()` で通知され、復旧可能な場合は
解析を続けます。入力ストリームを切り替える際は `reset()` を呼び出してください。
サービスとトラックの選択方針は維持されます。

音声トラックは、通知されたチャンネル構成、component type、main-component flag、
sample rate を含む MH audio component metadata を `TrackInfo::audio` で公開します。
番組をまたいで packet ID が固定されていると仮定せず、この metadata からトラックを
選択してください。

## データ放送アプリケーションと仮想ファイル

アプリケーションリソースの収集はデフォルトで有効です。demuxer はメディアの
アクセスユニットを出力しながら、application signalling、data-directory table、
asset-management table、および順不同で届く data unit を完全なファイルへ
組み立てます。`Sink` には `onApplicationState`、`onApplicationResource`、
`onApplicationResourcesReset` イベントが届きます。AIT の entry path が存在すると
アプリケーションは `Ready` になりますが、参照先のほかのファイルはその後も放送
carousel から到着する場合があります。

完成した byte 列は demuxer 内に無期限で保持されず、sink へ move されます。
ネイティブのホストでは、thread-safe な `ApplicationResourceStore` に保存できます。
その `get`、`list`、`waitFor` は loopback HTTP／WebView adapter での利用を想定して
います。

```cpp
class ReceiverSink final : public tlvdemux::Sink {
public:
    tlvdemux::ApplicationResourceStore files;

    void onApplicationState(const tlvdemux::ApplicationState& state) override {
        files.onApplicationState(state);
    }
    void onApplicationResource(tlvdemux::ApplicationResource&& resource) override {
        files.onApplicationResource(std::move(resource));
    }
    void onApplicationResourcesReset() override {
        files.onApplicationResourcesReset();
    }
    // 通常どおり、必須の media/error callback 4 個も実装します。
};
```

store 自体は socket や HTTP に依存しません。ネイティブアプリケーションは別の
server を `127.0.0.1` に bind し、`files.waitFor(context_id, path, timeout)` で
request に応答できます。WASM の利用側では通常、同じイベントを JavaScript の
`Map` に保持し、Service Worker 経由で公開します。

リソース収集は `Limits::collect_application_resources` で無効化できます。
`Limits` は pending item の個数／byte 数、catalogue size、展開後の file size にも
上限を設けるため、不正な carousel によってメモリーが無制限に増加することは
ありません。

## ストリームを調査する

```sh
./build/tlvdemux-inspect --list test.tlv
./build/tlvdemux-inspect --trace-au test.tlv
./build/tlvdemux-inspect --video video.hevc --audio audio.loas \
  --subtitle subtitle.ttml test.tlv
./build/tlvdemux-inspect --audio secondary.loas \
  --audio-packet-id 0xf311 test.tlv
```

検証用の入力を収録する際は、Mirakurun の raw 4K 経路を `decode=0` で使用します。

```sh
curl 'http://MIRAKURUN/api/services/SERVICE_ID/stream?decode=0' > test.tlv
```

同じ種類のトラックが複数ある場合、診断用 dumper は最初に検出した対応トラックを
書き出します。`--trace-au` では、引き続き出力されたすべてのトラックを表示します。

ライブラリは、必要な B61 descrambling が `Demuxer::push()` へ入力される前に
完了していることを前提とします。検証環境では、Mirakurun の `decode=0` が
MMT/TLV ストリームを維持しつつ、tuner/frontend 経路から利用可能な media payload
が渡されます。B61 message-authentication metadata は解析されるため、末尾に付加された
authentication code が media payload の一部として露出することはありません。
ただし、暗号学的な検証自体は呼び出し側の責任です。

## WebAssembly

npm から、ビルド済みの単一ファイル WebAssembly パッケージをインストールします。

```sh
npm install tlvdemux
```

パッケージは CommonJS から直接使用でき、ESM 対応 bundler で一般的な default import
interop にも対応します。

```js
import createTlvDemuxModule from "tlvdemux";

const module = await createTlvDemuxModule();
const demuxer = new module.TlvDemuxer({
  onTrack: track => console.log(track),
  onAccessUnitView: unit => consumeSynchronously(unit),
  onError: error => console.warn(error),
});
```

MSE プレーヤーでは、`mseMaxAudioChannels` によって、AAC の設定を書き換えずに
ブラウザー側の上限を超えるチャンネル構成を除外できます。たとえば `6` を指定すると
モノラルから 5.1ch までは維持し、22.2ch AAC の init segment は出力しません。
`onTrack` の `track.audio.channels` を使って、互換性のある別のトラックを選択して
ください。省略時または `0` の場合、remuxer はチャンネル数を制限しません。

#### BS8K の 22.2ch 音声を除外する

BS8K の番組によっては、AAC の `channel_configuration=13` で表される
22.2ch（24 チャンネル）音声が送出されます。Chromium 系ブラウザーはこの構成を
MSE で受け付けず、音声の `appendBuffer()` が MediaError になることがあります。
ブラウザー再生では次のように上限を 6 チャンネルにすると、モノラルから 5.1ch
までは通し、22.2ch の MSE init segment は出力しません。

```js
let selectedAudio = false;
const demuxer = new module.TlvDemuxer({
  mseMaxAudioChannels: 6,
  onTrack(track) {
    const channels = track.audio?.channels ?? 0;
    if (!selectedAudio && track.kind === "audio" &&
        (channels === 0 || channels <= 6)) {
      selectedAudio = true;
      demuxer.selectTrack("audio", track.trackId);
    }
  },
  onMseInit: init => appendInitSegment(init),
  onMseSegment: segment => appendMediaSegment(segment),
});
```

この設定は 22.2ch を 5.1ch にダウンミックスするものではなく、非対応の音声を
MSE に渡さないための安全策です。複数の音声トラックがある場合は
`track.audio.channels` を見て 5.1ch またはステレオの代替トラックを選択して
ください。省略時または `0` の場合、チャンネル数による制限は行いません。

module、callback、event、duration probe、recording index の TypeScript 型定義も
含まれます。npm パッケージには WebAssembly binary を埋め込んだ生成済み wrapper が
含まれるため、利用側に Emscripten は不要で、別の `.wasm` request も発生しません。

### iOS／iPadOS Safari

WASM demuxer 自体は、公開 API で使用する `BigInt` を含め、現在の iOS Safari で
動作します。ただし、player integration は標準の `MediaSource` constructor が
存在することを前提にできません。iOS では互換性のある `ManagedMediaSource` API が
公開されます。constructor を一度選択し、capability check と生成の両方で同じものを
使用してください。

```js
const BrowserMediaSource = globalThis.ManagedMediaSource || globalThis.MediaSource;
if (!BrowserMediaSource?.isTypeSupported(mime)) throw new Error(`Unsupported: ${mime}`);
const mediaSource = new BrowserMediaSource();
```

object URL を video element に設定する前に `sourceopen` listener を登録し、その後で
attach して再生を開始します。`demo/demo.js` はこの経路を実装しています。
`demo/ios-compat.html` は小さな feature／end-to-end 診断ページで、WASM、HEVC/AAC、
MSE/MMS、SourceBuffer の結果を個別に表示します。

iOS Simulator を ManagedMediaSource 再生の最終判断に使用しないでください。
WebKit bug 266764 では、Simulator が API を公開しながら source を open しない場合が
あると説明されています。SourceBuffer の段階は実機の iPhone／iPad で確認して
ください。WebKit の ManagedMediaSource integration example も参照してください。

- https://webkit.org/blog/15036/how-to-use-media-source-extensions-with-airplay/
- https://bugs.webkit.org/show_bug.cgi?id=266764

### npm パッケージをビルドする

Emscripten で browser／worker 用 wrapper をビルドします。

```sh
nix-shell
emcmake cmake -S . -B build-wasm -G Ninja \
  -DBUILD_SHARED_LIBS=OFF -DTLVDEMUX_BUILD_TOOLS=OFF
cmake --build build-wasm --target tlvdemux-wasm
```

`nix-shell` 内で `npm run build` を実行すると、同じ release build を行い、生成物を
`dist/tlvdemux.js` へコピーします。`npm pack --dry-run` は release build と WASM
smoke test を行ってから、実際に公開されるファイルを表示します。

生成物は単独で完結する `build-wasm/tlvdemux.js` です。WebAssembly binary は
埋め込まれており、別の `.wasm` request は発生しません。通常の script として
読み込み、非同期に demuxer を生成します。

```js
const module = await createTlvDemuxModule();
const demuxer = new module.TlvDemuxer({
  onTrack: track => console.log(track),
  onEventInfo: event => console.log(event.title, event.startTimeUnixMilliseconds),
  onStreamEvent: event => console.log(event.eventMessageTag, event.messageId),
  onAccessUnitView: unit => consumeSynchronously(unit),
  onApplicationState: application => console.log(application.state),
  onApplicationResourceView: resource => console.log(resource.path),
  onError: error => console.warn(error),
});

demuxer.push(chunk); // Uint8Array; WASM memory にコピーされます
demuxer.flush();
demuxer.delete();
```

buffer を管理済みの loader では、`_malloc`、`_free`、`HEAPU8`、
`pushFromHeap(address, size)` により再利用可能な heap-buffer 経路を利用できます。
JavaScript では、64-bit の offset、timestamp、track ID を `BigInt` で受け取ります。
MH-EIT の current/following および schedule entry は `onEventInfo` で通知されます。
`tableId === 0x8b` かつ `sectionNumber` が 0／1 の event は、その service の
present／following event です。

ARIB STD-B60 の EMT message は `onStreamEvent` で通知されます。event には、
MPT で通知された EMT tag、group／id／version、private byte、raw time-mode field が
含まれ、receiver は demux／read-ahead clock ではなく playback clock に合わせて
timed message を発火できます。`rawMessageId` は B60 の 16-bit descriptor field を
保持し、その上位 octet は `messageId`、下位 octet は `messageVersion` として B62
application に公開されます。

`onAccessUnitView` は media output のコピーを省きますが、その `data` view は callback
の実行中だけ有効なので、同期的に消費する必要があります。callback の後も保持する
場合は、所有された `Uint8Array` copy を返す `onAccessUnit` を使用してください。
`onApplicationResourceView` も同じく callback 中だけ有効です。所有された copy が
必要な場合は `onApplicationResource` を使用します。

`TlvDemuxer` は `ApplicationResourceStore` も所有します。
`applicationResources()` で file の一覧を取得し、
`applicationResource(contextId, path)` で所有された file を取得できます。
`applications()` は現在の application state を返し、
`applicationEntry(contextId)` は ready 状態の entry document を解決します。
これにより、path validation、version replacement、entry resolution の規則を
各 browser loader で重複実装せず、C++／WASM 側に集約できます。

収録した stream を使う application-resource WASM integration test は、次のように
実行します。

```sh
node tests/wasm_application_resources.mjs build-wasm/tlvdemux.js test.tlv
```

`DurationProbe` は file や HTTP client を所有せずに、先頭／末尾への高速な range read
を制御します。既知の file size で開始し、`nextRange()` が返す各 object に対して
request を実行し、取得した byte 列をそのまま `pushRange()` へ渡します。
成功した `duration()` の `status` は `"complete"` です。失敗は `state()` と
`failure()` で明示され、file 全体の download へ暗黙に fallback することは
ありません。native の `tlvdemux-probe INPUT` tool も同じ protocol を使用します。

録画で正確に seek するには、stream 全体を入力する前に `startIndex(false)` を呼び、
実際の EOF で `finalizeIndex()` を呼びます。`seekPointsFor(targetUs)` は、対象時刻を
挟む RAP checkpoint を返します。`first.signallingOffset` へ移動してそこから入力し、
出力された RAP から decode を開始し、指定時刻以降で最初の frame を表示します。

### ブラウザーデモ

隣接する `libaribhtml5` receiver SDK と `build-wasm/tlvdemux.js` をビルドし、
repository root を配信して `/demo/` を開きます。

```sh
(cd ../libaribhtml5 && pnpm build:sdk)
node demo/server.mjs
```

同梱の development server は、duration probe と録画 seek に必要な `206` および
`Content-Range` response に対応しています。Python の簡易
`python3 -m http.server` は必要な Range 動作を提供しないため、この demo には
適しません。

demo はローカルの MMTS file または HTTP URL を受け取り、duration を probe してから、
選択された HEVC／AAC track を Media Source Extensions で再生します。WASM が収集した
application resource は、`libaribhtml5` に含まれる同一 origin の Service Worker VFS
を通じ、sandbox 化された data-broadcast iframe に公開されます。receiver API、
video-plane 処理、document preparation、内蔵 ROM sound、remote-control の動作も
`libaribhtml5` が提供し、外部 application URL は引き続き block されます。

ローカル file では `Blob.slice()` を使用します。remote file は正しい `206` と
`Content-Range` response を返す必要があります。Live mode は duration probe と seek
を行わず、通常の streaming `GET` を使い、Media Source を上限のない timeline として
公開します。有効な Range response を返さない HTTP URL は自動的に Live mode へ
fallback します。

demo は意図的に小さく保った fMP4／MSE layer を持ち、実行時には mmts.js に依存
しません。ただし、ブラウザー側の HEVC MSE 対応は必要です。

demux と fMP4 remux は `demo/demux-worker-runtime.js` で実行されます。main thread は
transferable buffer として input chunk を送り、MSE init segment、media segment、
subtitle payload、application file、小さな control event だけを受け取ります。
`demo/worker-tlvdemux.js` が RPC facade を担い、`demo/demux-worker-protocol.js` が
共有 message name を定義します。この 3 つの責務を分離することで、player UI、
transport protocol、worker 側の demux lifecycle を個別に変更できます。

再現可能な WASM throughput benchmark は次のように実行します。

```sh
npm run benchmark:wasm -- build-wasm/tlvdemux.js test.tlv 268435456
```

demux のみ、および demux と MSE を組み合わせた throughput、callback／segment 数、
output byte 数、観測された WASM heap size の最大値を表示します。hot path の
ownership map、測定方法、regression checklist は
[`docs/performance.md`](docs/performance.md) を参照してください。

## 現在の対応範囲

Version 0.1 は、検証用 stream で確認した ARIB broadcast subset に対応します。
具体的には、4 種類すべての HCfB compressed-IP mode（`0x20`、`0x21`、`0x60`、
`0x61`）、MMTP signalling と fragmented media、HEVC Annex B、AAC-LATM/LOAS、
ARIB STD-B62 TTML です。録画向け helper は、上限付きの duration probe、疎な RAP
index、録画先頭を基準とする再配置を提供します。CAS／descrambling、decoder と
TTML rendering、永続的な index serialization、汎用的な ISO MMT は現在の
ライブラリの対象外です。
