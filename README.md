# chimmygochi

BT21のBABY CHIMMYを主人公とする計画の、Waveshare ESP32-S3-Touch-AMOLED-2.16向け育成ゲーム・待ち受け・時計。
Arduino / LVGLによる実機用プロトタイプを収録しています。
現在の絵・アニメーション・音は独自の仮素材で、CHIMMYの公式素材ではありません。

## 使い方

- 下の「遊戲 / 待機 / 時鐘 / 設定」で画面を切り替えます。
- 「餵食」で食事、「玩耍」でボール・泡遊び、「換裝」で3種類の見た目を選べます。
- 「睡覺 / 起床」で睡眠・起床。死亡・病気はありません。
- 時計は香港時間、UIは繁体字中国語です。
- 点灯中は全画面の上部に電池残量の推定値を表示します。未接続・取得エラーは数値にしません。
- 無操作2分で消灯。画面タッチまたはPWR短押しで復帰します。
- **電池仕様が未確認のため、主電池・バックアップ電池の充電は無効化しています。**

[詳しい操作・製作・素材差し替え・復旧手順](docs/operation.md)

## ビルド・転送

Windows または macOS、Node.js 22.12以上、Arduino IDE 2（同梱CLI）/ ESP32 core 3.3.12が必要です。
既存Flashのバックアップと接続先の確認を済ませてから書き込んでください。

```sh
npm ci
npm run setup
npm run assets
npm run check
npm run build
npm run upload
npm run device -- time
npm run device -- install
npm run device -- status
```

USBポートはESP32-S3（VID 303A）を自動検出します。複数ある・見つからない場合は
環境変数 `PORT`（例：`COM3`、`/dev/cu.usbmodem2301`）を指定します。素材転送は既存ファイルを上書きせず、
新規パックの検証後に切り替えます。SDカードのフォーマットは行いません。

## 設計資料

- [実装版の説明・現在の制限](docs/operation.md)
- [画像生成AI用プロンプト集（キャラクター12枚・背景・衣装違い）](docs/asset-prompts.md)
- [調査結果・根拠・ハードウェアの確認状況](docs/research.md)
- [暫定技術設計書・開発計画](docs/design.md)

設計書の全機能・性能目標を達成した完成版ではありません。
実機での指タッチ、音の聞こえ方、電池駆動時間などは人による確認が必要です。
