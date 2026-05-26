# firmware/

過去にビルドした `.uf2` のアーカイブ。各サブディレクトリは「どのブランチのどのコミットから出した firmware か」を示す。実機にロールバックしたいときの逃げ場として残してある。

## ディレクトリ命名規則

`<branch-slug>-<short-sha>/`

- ブランチ名のスラッシュは `-` に置換する。例: `feat/rgb-indicator-stage1` → `feat-rgb-indicator-stage1`
- short-sha は 7 桁。`git log --oneline` の左端と合わせる
- ブランチ名が説明的でない場合（例: バグ切り分け用）は、コミット sha を省いて目的を表す名前を使ってもよい (例: `diag-issue-11`)

## 各 `.uf2` の用途（[build.yaml](../build.yaml) の `artifact-name` と対応）

| ファイル | 焼き先 | 用途 |
|---|---|---|
| `cornix_left_default_nosd.uf2` | **左半** | 通常運用ファーム。central。`studio-rpc-usb-uart nrf52840-nosd` snippet 入りで ZMK Studio が使える |
| `cornix_right_nosd.uf2` | **右半** | 通常運用ファーム。peripheral |
| `cornix_reset.uf2` | 右半 (左にも焼けるが想定は右) | `settings_reset` シールド。NVS の split ペアリング情報をクリアするための一時ファーム。焼いて一度起動した直後に、上の通常ファームを焼き直すこと。これ単体で運用してはいけない |

`*_nosd` は SoftDevice なし版という意味。v2.3 以降このリポジトリは SoftDevice 復元不要なパーティション構成になっているので、`*_nosd` がそのまま動く。

## 焼く手順（CLAUDE.md 抜粋）

1. **データ転送可能** USB-C ケーブルで接続（充電専用ケーブル不可）
2. リセットボタン（pogo pin 横）を 2 回素早く押す → `CORNIX` または `NO NAME` というマスストレージが PC に現れる
3. 該当する `.uf2` を D&D
4. **左右両方を同じディレクトリの `.uf2` から焼く**（central/peripheral プロトコル整合のため。片半だけ更新すると split が動かなくなった事例あり）
5. ファイル転送中に「取り出しに失敗」系のエラーが出るのは正常（ファーム書き込み直後に再起動するため）

## 既存のディレクトリ

| ディレクトリ | 内容 |
|---|---|
| `main-41a468d` | main、Actions の push trigger 修正 ([41a468d](https://github.com/numachang/zmk-keyboard-cornix-jp/commit/41a468d)) 後の最初の動作確認用 |
| `main-abe5248` | main、非 Base レイヤの BT_SEL/BT_CLR キー配置確定 ([abe5248](https://github.com/numachang/zmk-keyboard-cornix-jp/commit/abe5248)) 後 |
| `diag-issue-11` | 右側スリープから復帰しない問題 (#11) の原因切り分け用ビルド + シリアルログ (`cornix-sleep-resume.log`) |
| `feat-rgb-indicator-stage1-406c99e` | RGB indicator 開発 (#2) の stage B1 — 起動時に LED0=暗赤・LED1=暗緑を 200 ms 点灯するだけの最小実装 |

## 整理ルール

- ロールバック先として **main の直近 2〜3 個** は必ず残す
- 開発ブランチのスナップショットは、PR がマージされて main に取り込まれたら削除して良い
- `diag-*` のような切り分け用ビルドは、対象 issue が closed になってから削除
