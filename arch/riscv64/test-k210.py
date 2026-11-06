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
    # 存在しない名前では、ハングもクラッシュもせず
    # 「無し」で返り、シェルが生き続けること
    # (SD が読めない状態でも同じ経路で -1 になる)
    ok = command("cat /sd/NOSUCH.TXT", "cat: no such file")
    ok = command("ls", "nish\\.elf") and ok
    report("K7 /sd failure keeps the shell alive", ok)

    print("== scenario K8: fs stage 4 (directories + cwd) ==")
    # 実カードのルート一覧 (DCIM / MISC / BOOT.PY) が
    # ls /sd で出ること、/sd へ cd して相対パスで
    # BOOT.PY が読めること、RAM 側の mkdir / cd / pwd
    ok = command("ls /sd", "BOOT\\.PY")
    ok = command("ls /sd", "DCIM") and ok
    ok = command("cd /sd", "nish>") and ok
    ok = command("cat BOOT.PY", "fpioa_manager") and ok
    ok = command("cd /", "nish>") and ok
    ok = command("mkdir hw4", "created hw4") and ok
    ok = command("cd hw4", "nish>") and ok
    ok = command("pwd", "/hw4") and ok
    ok = command("cd /", "nish>") and ok
    ok = command("rmdir hw4", "removed hw4") and ok
    report("K8 fs stage 4 (ls /sd, cd, relative cat)", ok)

    print("== scenario K9: exec multilevel (chain) ==")
    # 実機でも多段 exec が動くこと: chain.elf が 3 段まで自分を
    # exec し、ステータスが伝わり (最終 6)、親の fd が子の後も
    # 読めること。burn はスタックの fault 伸長 (ドラフト仕様の
    # アクセスフォルト経由) の確認も兼ねる
    cur = log_total()
    send_chunked("run chain.elf 3")
    call_tool("serial_send", {"text": "", "newline": True})
    ok = expect("chain 0 start", 10000, cursor=cur)
    ok = expect("burn=4160", 5000, cursor=cur) and ok
    ok = expect("fdok= fro", 5000, cursor=cur) and ok
    ok = expect("run: exit status 6", 5000, cursor=cur) and ok
    report("K9 exec multilevel (chain: depth / status / fd)", ok)

    print("== scenario K10: kernel log (dmesg) ==")
    # dmesg で起動時の記録 (プールの総数 1280 = 5MB 分) が
    # 読み返せることと、chain の deep 25 (深さ上限の拒否)
    # の理由がログに残っていて、あとから dmesg で追えること。
    # command() はカーソルを送信前に取るので、2 回目の
    # dmesg での一致は「ログからの読み返し」を意味する
    ok = command("dmesg", "page: pool total 1280, free 1280")
    ok = command("run chain.elf deep 25", "deep denied",
                 timeout_ms=15000) and ok
    ok = command("dmesg", "exec: denied, depth limit") and ok
    report("K10 kernel log (dmesg: boot records + exec denial)", ok)

    print("== scenario K11: pool exhaustion is logged ==")
    # eat.elf は 1 段で約 250 ページ食べるので、K210 の
    # プール (1280 枚) では数段で枯渇して exec が -1 に
    # なる (深さ上限 16 より先に枯渇する)。枯渇する箇所は
    # ページ数の端数で ELF のセグメント確保かスタック
    # 準備か変わるので、ログの行は "out of pages" から
    # "pool free 0" までの並びで見る。起きた瞬間の表示では
    # なく、失敗のあとに dmesg で読み返せることを確認
    # する (カーソル方式なので、一致はログ側の記録を
    # 意味する)
    ok = command("run eat.elf 12", "eat: exec failed",
                 timeout_ms=20000)
    ok = command("dmesg", "out of pages.*pool free 0") and ok
    report("K11 pool exhaustion logged (eat.elf + dmesg)", ok)

    print("== scenario K12: uptime syscall ==")
    # uptime.elf が syscall 31 (起動以来のミリ秒) を呼んで、
    # 単調 (減らない)・500ms の待ちでの進み・起動直後の
    # 値の範囲をプログラム側で判定し "uptime ok" と出す
    # (詳細は uptime.c)。実機のタイマは実時間なので、
    # 待ちは実時間の 500ms 分になる。FAIL のときは
    # "uptime ok" が出ないので、そちらの一致で判定する
    cur = log_total()
    send_chunked("run uptime.elf")
    call_tool("serial_send", {"text": "", "newline": True})
    ok = expect("uptime t0=", 10000, cursor=cur)
    ok = expect("uptime t1=", 8000, cursor=cur) and ok
    ok = expect("uptime ok", 5000, cursor=cur) and ok
    report("K12 uptime (monotonic / advances / sane range)", ok)

    print("== scenario K6: exit to kernel console ==")
    ok = command("exit", "back from user program")
    report("K6 exit to kernel console", ok)

    print("== scenario K13: wall clock (K210 RTC) ==")
    # カーネルコンソールの 't' キー (doc/time.md)。未設定時の
    # 表示を見てから、設定 → 数秒待って読み戻しの往復が
    # 数秒以内の誤差で一致することを見る。未設定時は
    # "(RTC not set)" になるのが実測だが、以前の設定が
    # 残っている場合もありうるので、最初の表示はどちら
    # でもよい形で確認する (値そのものは dev-notes に記録)
    cur = log_total()
    send_chunked("t")
    ok = expect(r"wall: (\(RTC not set\)|20[0-9][0-9]-[0-9][0-9]-[0-9][0-9] [0-9:]+ UTC)",
                10000, cursor=cur)
    ok = expect(r"set time\?", 5000, cursor=cur) and ok
    call_tool("serial_send", {"text": "", "newline": True})
    ok = expect("time: kept", 5000, cursor=cur) and ok
    # 設定する ('t' → プロンプト → 日時の行)
    cur = log_total()
    send_chunked("t")
    ok = expect(r"set time\?", 8000, cursor=cur) and ok
    send_chunked("2032-03-04 05:06:07")
    call_tool("serial_send", {"text": "", "newline": True})
    ok = expect("time set: 2032-03-04 05:06:07", 10000, cursor=cur) and ok
    # 3 秒待って読み直す。秒が 08〜19 の範囲に進んでいる
    # こと (設定値 +1〜+12 秒) で往復の一致とする
    time.sleep(3)
    cur = log_total()
    send_chunked("t")
    ok = expect(r"wall: 2032-03-04 05:06:(0[89]|1[0-9])", 10000,
                cursor=cur) and ok
    call_tool("serial_send", {"text": "", "newline": True})
    ok = expect("time: kept", 5000, cursor=cur) and ok
    report("K13 wall clock (RTC set + read back)", ok)

    print("== scenario K14: FP smoke (fptest.elf, lp64d) ==")
    # カーネルコンソールから Nish に戻り ('s' キー)、
    # fptest.elf を実行する。fptest は、プログラム開始時に
    # FS が Initial 化されている前提で float / double の
    # 四則と変換を実命令で確かめ、段階ごとに ok を出す。
    # K210 では違法命令 (例外 2) が medeleg で S へ委譲
    # されず M シムの扱いになるので、FS が効いていなければ
    # ok は出ない (異常終了するか M シムで止まる)。逆に
    # ここが通れば、mshim の mstatus.FS 設定込みで実機の
    # FP が実際に動いたことの実測になる
    cur = log_total()
    send_chunked("s")
    ok = expect("Nish, the KosabaOS shell", 10000, cursor=cur)
    cur = log_total()
    send_chunked("run fptest.elf")
    call_tool("serial_send", {"text": "", "newline": True})
    ok = expect("fptest: float ok", 10000, cursor=cur) and ok
    ok = expect("fptest: double ok", 5000, cursor=cur) and ok
    ok = expect("fptest: conv ok", 5000, cursor=cur) and ok
    ok = expect("fptest: done", 5000, cursor=cur) and ok
    report("K14 FP smoke (float/double ops on K210)", ok)

    print("== summary ==")
    if fails == 0:
        print("ALL PASS")
    else:
        print(f"{fails} scenario(s) failed")
    return fails


if __name__ == "__main__":
    sys.exit(main())
