# libaribtlv

[English](README.md) | 日本語

`libaribtlv` は、復号済み ARIB MMT/TLV ストリームをインクリメンタルに
解析する C++20 ライブラリです。ネイティブ受信機、コマンドラインツール、
WebAssembly アダプターで共有する次のコア機能を提供します。

- TLV 再同期、圧縮 IP コンテキスト、MMTP 分割・集約処理
- PA/M2/MPT、MH-AIT、EIT、SDT、TOT、EMT、B60 データ伝送シグナリング
- HEVC、AAC-LATM/LOAS、ARIB STD-B62 TTML アクセスユニット
- ARIB-HTML5 アプリケーションリソースの組み立てとメモリ内ストア
- 録画インデックス、シークポイント、長さ計測、Range ベースの probe

プレイヤーのセッション制御、MSE/fMP4 remux、JavaScript binding、CLI、
ブラウザ demo は sibling の `tlvdemux` プロジェクトに置きます。

## ビルドとテスト

```sh
nix-shell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

既定では共有ライブラリ `libaribtlv.so.0` または
`libaribtlv.0.dylib` を生成します。静的ライブラリが必要な場合は
`-DBUILD_SHARED_LIBS=OFF` を指定してください。標準ライブラリ以外の依存は
Zlib のみで、Emscripten では組み込みの zlib port を利用します。

```sh
cmake --install build --prefix /desired/prefix
```

利用側は `find_package(aribtlv CONFIG REQUIRED)` を行い、
`aribtlv::aribtlv` にリンクします。

## 使用例

```cpp
#include <aribtlv/demuxer.hpp>

class Receiver final : public aribtlv::Sink {
public:
    void onService(const aribtlv::ServiceInfo&) override;
    void onTrack(const aribtlv::TrackInfo&) override;
    void onAccessUnit(aribtlv::AccessUnit&&) override;
    void onError(const aribtlv::Error&) override;
};

Receiver receiver;
aribtlv::Demuxer demuxer(receiver);
demuxer.push(data, size);
demuxer.flush();
```

`push()` は任意のチャンク境界を同期的に処理し、入力ポインターを保持しません。
同じソース内のシークには `reposition()`、ソース交換には `reset()`、実際の
入力境界または EOF には `flush()` を使用します。

公開 ABI は C++20 です。動的リンクする場合は互換性のあるコンパイラと
C++ 標準ライブラリを使用してください。

## ライセンス

MIT
