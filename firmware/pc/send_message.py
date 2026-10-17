#!/usr/bin/env python3
"""ESP32 環境センサモニタへ一言メッセージを TCP 送信する簡易クライアント。

ESP32 側は USR-ES1(W5500) で TCP サーバ (既定 192.168.1.50:5000) を立てている。
本スクリプトは 1 行を改行区切りで送り、ESP32 からの "OK" 応答を表示する。

使い方:
    python send_message.py "hello"                       # 1 回送信
    python send_message.py "hello" --host 192.168.1.50 --port 5000
    python send_message.py                               # 対話モード (空行で終了)

依存: Python 3 の標準ライブラリのみ (Windows / macOS / Linux で動作)。
"""
import argparse
import socket
import sys


def send(host: str, port: int, text: str, timeout: float = 5.0) -> str:
    """1 行を送信し、ESP32 からの応答 (改行除去済み) を返す。"""
    payload = (text.rstrip("\r\n") + "\n").encode("utf-8")
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.sendall(payload)
        sock.settimeout(timeout)
        try:
            return sock.recv(64).decode("utf-8", "replace").strip()
        except socket.timeout:
            return ""


def main() -> None:
    parser = argparse.ArgumentParser(description="Send a one-line message to the ESP32 monitor.")
    parser.add_argument("message", nargs="*", help="送信テキスト (省略時は対話モード)")
    parser.add_argument("--host", default="192.168.1.50", help="ESP32 の IP (既定 192.168.1.50)")
    parser.add_argument("--port", type=int, default=5000, help="TCP ポート (既定 5000)")
    args = parser.parse_args()

    if args.message:
        text = " ".join(args.message)
        try:
            reply = send(args.host, args.port, text)
            print(f"sent: {text!r}  reply: {reply or '(no reply)'}")
        except OSError as exc:
            print(f"error: {exc}", file=sys.stderr)
            sys.exit(1)
        return

    # 対話モード
    print(f"# interactive -> {args.host}:{args.port}  (空行 / Ctrl-D で終了)")
    try:
        for line in sys.stdin:
            line = line.strip()
            if not line:
                break
            try:
                reply = send(args.host, args.port, line)
                print(f"sent: {line!r}  reply: {reply or '(no reply)'}")
            except OSError as exc:
                print(f"error: {exc}", file=sys.stderr)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
