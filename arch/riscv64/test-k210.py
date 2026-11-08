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
import re
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

    print("== scenario K15: SD write ==")
    # /sd への書き込み対応の実機確認 (doc/sd-write-design.md
    # 段4)。カードは中身を退避済みのカメラカードで、触るのは
    # テストファイルの作成と削除だけ (既存ファイルは変更
    # しない)。書き込みの前後で既存の BOOT.PY が無傷に
    # 読めることもあわせて見る。Nish には K14 の流れで
    # 入ったままの状態から始める
    ok = command("write /sd/K15TEST.TXT k210-sd-write",
                 "wrote /sd/K15TEST\\.TXT")
    ok = command("cat /sd/K15TEST.TXT", "k210-sd-write") and ok
    ok = command("cat /sd/BOOT.PY", "fpioa_manager") and ok
    ok = command("rm /sd/K15TEST.TXT",
                 "removed /sd/K15TEST\\.TXT") and ok
    ok = command("cat /sd/K15TEST.TXT", "cat: no such file") and ok
    report("K15 SD write (create/write/read-back/unlink, "
           "existing files intact)", ok)

    print("== scenario K20: SD exec ==")
    # /sd 上の ELF の実行の実機確認 (doc/sd-exec-design.md)。
    # K19 は sd-rmdir ブランチが使用済みのため K20。
    # copy.elf で RAM FS の uprog.elf を /sd へ運び (Nish の
    # write はテキスト前提でバイナリに使えない)、それを
    # run して挨拶と終了ステータス 7 ('e' キー) まで見る。
    # 終わったらコピーは消し、BOOT.PY が無傷なことも見る
    ok = command("run copy.elf uprog.elf /sd/UPROG.ELF",
                 "copied")
    cur = log_total()
    send_chunked("run /sd/UPROG.ELF")
    call_tool("serial_send", {"text": "", "newline": True})
    ok = expect("hello from user mode!", 15000, cursor=cur) and ok
    cur = log_total()
    send_chunked("e")
    ok = expect("run: exit status 7", 10000, cursor=cur) and ok
    ok = command("rm /sd/UPROG.ELF",
                 "removed /sd/UPROG\\.ELF") and ok
    ok = command("cat /sd/BOOT.PY", "fpioa_manager") and ok
    report("K20 SD exec (copy to /sd, run, exit status 7)", ok)

    print("== scenario K16: clock task (scheduler stage 2) ==")
    # スケジューラ段2 (doc/scheduler-plan.md): 時計タスクが
    # 常駐し、入力待ちの間も sleep から起き続けていることを
    # ps の起床回数 (wakes) で見る。Nish のプロンプトで
    # 数秒待つ → 'exit' でカーネルコンソールに落ちて 5 秒
    # 待つ → 's' で Nish に戻る、の流れで、両方の待ちを
    # またいで wakes が増えていることを確認する (待ちの
    # 前後で ps を 2 回取り、差を見る)。
    # LCD の秒表示が実際に進むところはシリアルからは
    # 見えないので、人間の目視の担当 (このシナリオは
    # シリアル側の証跡 = wakes と dmesg の記録まで)
    def ps_wakes():
        cur = log_total()
        send_chunked("ps")
        call_tool("serial_send", {"text": "", "newline": True})
        if not expect("wakes=", 10000, cursor=cur):
            return None
        r = call_tool("serial_read", {"cursor": cur})
        m = re.findall(r"task 3: (?:sleeping|ready) wakes=(\d+)",
                       r.get("text", ""))
        return int(m[-1]) if m else None

    w1 = ps_wakes()
    ok = w1 is not None
    ok = command("exit", "back from user program") and ok
    time.sleep(5)  # カーネルコンソールの入力待ち
    cur = log_total()
    send_chunked("s")
    ok = expect("Nish, the KosabaOS shell", 10000, cursor=cur) and ok
    w2 = ps_wakes()
    ok = w2 is not None and ok
    if w1 is not None and w2 is not None:
        print(f"   wakes: {w1} -> {w2} (across console wait)")
        if w2 - w1 < 3:
            ok = False
    ok = command("dmesg", "clock: wake") and ok
    report("K16 clock task (wakes advance across input waits)", ok)

    print("== scenario K17: job control (bg jobs) ==")
    # ジョブ制御 (スケジューラ段3、doc/jobs-design.md) の
    # 実機確認。Nish に入ったままの状態から: slow.elf を
    # bg で走らせるとプロンプトが戻り、走っている間に
    # 別のコマンドが打て、jobs に running と見え、fg が
    # 完了まで待つ。crash.elf のフォルトはそのジョブだけを
    # 終わらせ、Nish は無事 (fg が faulted を報告する)。
    # 実機はコマンド 1 本の往復に数秒かかる (デーモン経由の
    # 刻み送信) ので、slow は 80 回 (約 24 秒) と長めにし、
    # echo と jobs の 2 往復を終えてもまだ走っている長さに
    # する。fg の照合は「fg を送った後に slow done が出る」
    # 形になるよう、完了が fg の待ち窓の中に入る計算
    ok = command("run slow.elf 80 &", "job 1: slow\\.elf started",
                 20000)
    ok = command("echo during", "during", 20000) and ok
    ok = command("jobs", "job 1: slow\\.elf running", 20000) and ok
    ok = command("fg", "slow done", 60000) and ok
    ok = command("jobs", "no jobs", 20000) and ok
    ok = command("run crash.elf &", "job 1: crash\\.elf started",
                 20000) and ok
    ok = command("echo alive", "alive", 20000) and ok
    ok = command("fg", "fg: program faulted", 20000) and ok
    report("K17 job control (bg / jobs / fg / fault isolation)", ok)

    print("== scenario K18: gfx windows ==")
    # 画面表示 (doc/gfx-design.md) の実機確認。シリアルで
    # 追える範囲: gdemo.elf を前面で走らせて描画が配線
    # されていること、gjob.elf を bg で走らせると窓が開き、
    # fg で完了まで待つと窓が回収される (dmesg の
    # gfx: close) こと。LCD に窓・枠・フォーカスが実際に
    # 見えるかはシリアルから確認できないので目視の担当
    ok = command("run gdemo.elf", "gdemo: done", 30000)
    ok = command("run gjob.elf 30 &", "job 1: gjob\\.elf started",
                 20000) and ok
    ok = command("fg", "gjob done", 60000) and ok
    ok = command("dmesg", "gfx: close r=1", 20000) and ok
    report("K18 gfx windows (drawing + window lifecycle via dmesg)",
           ok)
    print("== scenario K19: SD rmdir ==")
    # /sd の rmdir 対応の実機確認 (doc/sd-write-design.md 追補)。
    # K18 は gfx-windows ブランチが使用済みのため K19 から。
    # K15 と同じく、触るのはテスト用ディレクトリの作成と
    # 削除だけ (既存ファイルは変更しない)。中にファイルが
    # ある間は rmdir が断られ、消せば通ること。前後で
    # BOOT.PY が無傷に読めることも見る
    ok = command("mkdir /sd/K19DIR", "created /sd/K19DIR")
    ok = command("write /sd/K19DIR/F.TXT k19",
                 "wrote /sd/K19DIR/F\\.TXT") and ok
    ok = command("rmdir /sd/K19DIR", "rmdir: cannot remove") and ok
    ok = command("rm /sd/K19DIR/F.TXT",
                 "removed /sd/K19DIR/F\\.TXT") and ok
    ok = command("rmdir /sd/K19DIR", "removed /sd/K19DIR") and ok
    ok = command("cat /sd/K19DIR/F.TXT", "cat: no such file") and ok
    ok = command("cat /sd/BOOT.PY", "fpioa_manager") and ok
    report("K19 SD rmdir (mkdir / non-empty refused / unlink / "
           "rmdir, existing files intact)", ok)

    print("== scenario K21: audio i2s (tone.elf) ==")
    # オーディオ実体 (K210 I2S、doc/audio-driver-design.md)
    # の実機確認。シリアルで追える範囲: tone.elf が正常
    # 終了することと、dmesg にドライバの要約行 (frames =
    # レート × 秒数 = 132300) と従来の close 行が出る
    # こと。音そのもの (出ているか、音程、音量) は
    # シリアルでは確認できないのでユーザーの耳の担当で、
    # 残件として報告する。tone は実時間で約 3 秒鳴る
    ok = command("run tone.elf", "tone: done", 60000)
    ok = command("dmesg", "audio: i2s frames=132300") and ok
    ok = command("dmesg",
                 "audio: close rate=44100 channels=1 "
                 "samples=132300") and ok
    report("K21 audio i2s (tone plays, i2s line via dmesg)", ok)

    print("== scenario K22: nami player (wav playback) ==")
    # nami (音声ファイル再生プレーヤー、doc/nami-design.md)
    # の実機確認。wavgen.elf で /sd にテスト WAV を作り
    # (申告 samples / fnv をシリアルから拾う)、nami で
    # 再生して、dmesg の "audio: close" 行と I2S の
    # frames 行が申告と一致することを見る。エラー系
    # (無いファイル / WAV でない / MP3) の終了ステータスも。
    # 音そのものはスピーカー未接続のため確認対象外で、
    # シリアル側の照合までが範囲 (K21 と同じ線引き)。
    # wavgen の書き込みは 176KB を SPI の SD へ書くので
    # 待ちは長めに取る。素材 (IwaC 製 nami) が埋め込まれて
    # いる前提 (開発環境の標準ビルド)
    cur = log_total()
    send_chunked("run wavgen.elf /sd/TEST.WAV")
    call_tool("serial_send", {"text": "", "newline": True})
    ok = expect("wavgen: done", 90000, cursor=cur)
    samples = fnv = None
    if ok:
        r = call_tool("serial_read", {"cursor": cur})
        m = re.search(r"wavgen: samples=(\d+) fnv=([0-9a-f]{16})",
                      r.get("text", ""))
        if m:
            samples, fnv = m.group(1), m.group(2)
            print(f"   wavgen declares: samples={samples} "
                  f"fnv={fnv}")
        else:
            ok = False
    if samples is not None:
        ok = command("run nami /sd/TEST.WAV", "nami: done",
                     60000) and ok
        ok = command("dmesg",
                     f"audio: close rate=44100 channels=1 "
                     f"samples={samples} fnv={fnv}") and ok
        ok = command("dmesg",
                     f"audio: i2s frames={samples}") and ok
        ok = command("run nami /sd/NOPE.WAV",
                     "run: exit status 1") and ok
        ok = command("run nami /sd/BOOT.PY",
                     "run: exit status 4") and ok
        ok = command("write /sd/FAKE.MP3 ID3-notreal",
                     "wrote /sd/FAKE\\.MP3") and ok
        ok = command("run nami /sd/FAKE.MP3",
                     "run: exit status 3") and ok
        ok = command("rm /sd/TEST.WAV",
                     "removed /sd/TEST\\.WAV") and ok
        ok = command("rm /sd/FAKE.MP3",
                     "removed /sd/FAKE\\.MP3") and ok
    ok = command("cat /sd/BOOT.PY", "fpioa_manager") and ok
    report("K22 nami player (wavgen -> nami, fnv via dmesg, "
           "error statuses)", ok)

    print("== summary ==")
    if fails == 0:
        print("ALL PASS")
    else:
        print(f"{fails} scenario(s) failed")
    return fails


if __name__ == "__main__":
    sys.exit(main())
