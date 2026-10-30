# example/002: FIDO2 USBセキュリティキー

KagigataDiskをUSBのセキュリティキー（パスキー）にするサンプルです。カード端子に触れるとタッチ扱いになります。

準備（ツールと `./tools/install.sh`）はリポジトリの [README](../../README.md) を見てください。

## 書き込み

> [!WARNING]
> 書き込むと、キーに登録済みのパスキーとPINはすべて消えます。

```bash
./tools/kagigata_flash.sh 002 --build
```

- LED1〜3が全点灯したら、カード端子に**3秒間触れ続けて**ください（セキュリティキーとして動いているときだけ）。
- `flashed 002` と出れば完了です。

## 使い方

1. PCのUSBに挿して、サイトでパスキーを登録・ログインします。
2. LED1が点滅したら、カード端子に触れます。

| LED | 意味 |
|---|---|
| LED1 点滅 | タッチ待ち |
| LED2 / LED3 | 送信 / 受信 |
| 全点灯 | 書き込みの確認中（3秒タッチ） |

```bash
./tools/kgfido.py info            # 状態を表示
./tools/kgfido.py touch --watch   # タッチが反応しないときの確認
```

## 自分用の設定で書き込む

USB IDやAAGUIDなどは `-D` で指定して書き込みます（指定した値は次回以降も使われます）。

```bash
./tools/kagigata_flash.sh 002 -DKGFIDO_AAGUID=$(uuidgen | tr -d -)
```

| 変数 | 既定値 | 内容 |
|---|---|---|
| `KGFIDO_AAGUID` | 全0 | 機種のID（`uuidgen` で作る） |
| `KGFIDO_USB_VID` / `KGFIDO_USB_PID` | `0xCAFE` / `0x4004` | USB ID（TinyUSBのサンプルから引用）。変えたら `tools/env.sh` とudevルールも合わせる |
| `KGFIDO_MANUFACTURER` / `KGFIDO_PRODUCT` | `TinyUSB` / `TinyUSB Device` | USBの名前 |
| `KGFIDO_TOUCH_PINS` | `0,1,2,7` | タッチに使うGPIO |
| `KGFIDO_ATTEST_KEY` / `KGFIDO_ATTEST_CERT` | なし（自己証明） | アテステーション鍵と証明書（下記） |

アテステーション付きで書き込む：

```bash
AAGUID=$(uuidgen | tr -d -)
./tools/fido_attest.sh $AAGUID
./tools/kagigata_flash.sh 002 -DKGFIDO_AAGUID=$AAGUID \
    -DKGFIDO_ATTEST_KEY=$PWD/example/002/attest/attest_key.hex \
    -DKGFIDO_ATTEST_CERT=$PWD/example/002/attest/attest_cert.der
```

## 仕様

- CTAP 2.0、ES256、PINプロトコル1。U2Fは非対応
- パスキーは最大32件。PINは8回間違えるとロック
- 秘密はキーが初回起動時に乱数（TRNG）で作ってflashに保存。誰がビルドしても同じにはなりません

## 注意事項

- サンプルです。秘密はflashに平文で保存しています。紛失・盗難時にも秘密を守る必要がある場合は、**RP2350のOTPを使ってください。OTPの設定は元に戻せません。**
- FIDO Allianceの認定は受けていません。USB IDはKagigataDiskに割り当てられたものではありません。
- 001から書き換えると、001のUSBドライブのファイルは消えます。
