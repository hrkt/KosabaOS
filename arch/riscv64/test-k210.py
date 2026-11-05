#!/usr/bin/env python3
# test-k210.py -- K210 実機版の自動検証 (test.sh の実機版)
#
# QEMU 用の test.sh と同じ考え方で、実機 (Sipeed Maix Dock) の
# Nish をシリアル経由で操作し、期待する応答が返るかを見る。
# 実機へのアクセスは Hatoba デーモン (board-daemon) 経由で、
# そのクライアントを subprocess で呼ぶ。
#
# 使い方:
#   ./test-k210.py            ビルド → flash → 全シナリオ
#   ./test-k210.py --no-flash flash を飛ばして対話だけ (起動済みのとき)
#
# クライアントの場所は環境変数 BOARD_DAEMON_CLIENT で差し替え可
# (既定は Muse の custom connector のもの)。
#
# 送信の作法 (実機で学んだこと):
#   K210 の UARTHS の受信 FIFO は 8 段しかない。長い文字列を
#   一気に送ると後半が溢れて落ちるので、コマンドは短く刻んで
#   間隔を空けて送る (デーモン側の pace 付き送信でもよいが、
#   ここではスクリプト側で刻むことで旧デーモンでも動かす)。

import base64
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CLIENT = os.environ.get(
    "BOARD_DAEMON_CLIENT",
    os.path.expanduser(
        "~/workspace/custom-connectors/custom.board-daemon/bin/client.py"))

fails = 0


def call_tool(name, args=None, timeout=60):
    cmd = ["python3", CLIENT, "call-tool", "--name", name]
    path = None
    if args is not None:
        # 大きな引数 (flash の base64) はファイル経由で渡す
        if len(json.dumps(args)) > 2000:
            import tempfile
            fd, path = tempfile.mkstemp(suffix=".json")
            with os.fdopen(fd, "w") as f:
                json.dump(args, f)
            cmd += ["--arguments-file", path]
        else:
            cmd += ["--arguments-json", json.dumps(args)]
    out = subprocess.run(cmd, capture_output=True, text=True,
                         timeout=timeout)
    if path:
        os.unlink(path)
    if out.returncode != 0:
        raise RuntimeError(f"{name} failed: {out.stderr.strip()[:200]}")
    return json.loads(out.stdout)


def log_total():
    """現在の受信ログ末尾 (カーソル) を取る。期待パターンの探索を
    「これから届く分」だけに絞るために使う (ログは累積なので、
    カーソル無しだと前回の起動の文字列に誤って一致する)。"""
    r = call_tool("serial_read", {"cursor": 2**62})
    return r.get("cursor", 0)


def send_chunked(text, wait=0.06, chunk=6):
    """コマンド文字列を刻んで送る (改行は付けない)。"""
    for i in range(0, len(text), chunk):
        call_tool("serial_send",
                  {"text": text[i:i + chunk], "newline": False})
        time.sleep(wait)


def expect(pattern, timeout_ms=10000, cursor=None):
    args = {"pattern": pattern, "timeout_ms": timeout_ms}
    if cursor is not None:
        args["cursor"] = cursor
    r = call_tool("serial_expect", args,
                  timeout=timeout_ms / 1000 + 15)
    return bool(r.get("matched"))


def command(cmd, pattern, timeout_ms=10000):
    """コマンドを送って、応答パターンを待つ。成否を返す。"""
    cur = log_total()
    send_chunked(cmd)
    call_tool("serial_send", {"text": "", "newline": True})
    return expect(pattern, timeout_ms, cursor=cur)


def report(name, ok):
    global fails
    print(f"{'PASS' if ok else 'FAIL'}: {name}")
    if not ok:
        fails += 1


def main():
    do_flash = "--no-flash" not in sys.argv

    if do_flash:
        print("== build (BOARD=k210) ==")
        r = subprocess.run(["./build.sh"], cwd=HERE,
                           env={**os.environ, "BUILD_NO_RUN": "1",
                                "BOARD": "k210"},
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout[-500:], r.stderr[-500:])
            print("FAIL: build")
            return 1
        print("build ok")

        st = call_tool("board_status")
        if not st.get("attached"):
            call_tool("board_attach")

        boot_cursor = log_total()
        print("== flash (SRAM boot) ==")
        data = open(os.path.join(HERE, "kernel.bin"), "rb").read()
        r = call_tool("flash_kflash",
                      {"file_b64": base64.b64encode(data).decode(),
                       "board": "dan", "sram": True, "timeout_sec": 60},
                      timeout=120)
        if not r.get("success_marker_found"):
            print("FAIL: flash", r)
            return 1
        print("flash ok")
        time.sleep(3)

    if not do_flash:
        boot_cursor = log_total()
    print("== scenario K1: boot + Nish prompt ==")
    report("K1 boot + Nish prompt",
           expect("hello from KosabaOS \\(k210\\)", 15000,
                  cursor=boot_cursor)
           and expect("nish>", 5000, cursor=boot_cursor))

    print("== scenario K2: ls / cat ==")
    ok = command("ls", "nish\\.elf")
    ok = command("cat hello.txt",
                 "Hello from the KosabaOS RAM filesystem\\.") and ok
    report("K2 ls / cat", ok)

    print("== scenario K3: run uprog.elf ==")
    # 挨拶と probe はほぼ同時に出る。カーソルは送信前に 1 回だけ
    # 取り、両方の expect で使い回す (途中で取り直すと、もう
    # 出てしまった probe を見逃す)
    cur = log_total()
    send_chunked("run uprog.elf")
    call_tool("serial_send", {"text": "", "newline": True})
    ok = expect("hello from user mode!", 10000, cursor=cur)
    ok = expect("data probe: 8 x", 5000, cursor=cur) and ok
    cur = log_total()
    send_chunked("q")
    ok = expect("nish>", 8000, cursor=cur) and ok
    report("K3 run uprog.elf (exec + data probe)", ok)

    print("== scenario K4: run failure (regression) ==")
    ok = command("run nosuch.elf", "run: cannot start")
    ok = command("ls", "nish\\.elf") and ok
    report("K4 run failure keeps the shell alive", ok)

    print("== scenario K5: write + cat + rm ==")
    ok = command("write memo.txt hw-test", "wrote memo\\.txt")
    ok = command("cat memo.txt", "hw-test") and ok
    ok = command("rm memo.txt", "removed memo\\.txt") and ok
    ok = command("cat memo.txt", "cat: no such file") and ok
    report("K5 write + cat + rm", ok)

    print("== scenario K7: /sd failure is graceful ==")
    # SD が読めない状態 (この基板は SCLK 線の物理故障で
    # 常にこの経路) でも、ハングもクラッシュもせず
    # 「無し」で返り、シェルが生き続けること。健全な基板
    # でも存在しない名前なら同じ結果になる
    ok = command("cat /sd/NOSUCH.TXT", "cat: no such file")
    ok = command("ls", "nish\\.elf") and ok
    report("K7 /sd failure keeps the shell alive", ok)

    print("== scenario K6: exit to kernel console ==")
    ok = command("exit", "back from user program")
    report("K6 exit to kernel console", ok)

    print("== summary ==")
    if fails == 0:
        print("ALL PASS")
    else:
        print(f"{fails} scenario(s) failed")
    return fails


if __name__ == "__main__":
    sys.exit(main())
