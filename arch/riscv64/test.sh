#!/bin/sh
# test.sh -- KosabaOS の動作確認をまとめて行うスクリプト
#
# ビルドしてから QEMU を 3 つのシナリオで起動し、出力を
# 期待する文字列の「順序どおりの出現」で検証する。
# tick 行は割り込みのタイミングで割り込むので、検証前に
# 取り除いて文字の流れだけを見る。
#
#   シナリオ A: 起動 (ページング有効化、hello、tick)
#   シナリオ B: カーネルの echo と 'x' のページフォルトデモ
#   シナリオ C: 'u' でユーザープログラム (挨拶、ユーザー側 echo、
#               'q' で復帰、カーネル側 echo の再開)
#   シナリオ D: 'm' でマルチタスクのデモ (協調で ABAB 交互、
#               プリエンプティブで塊ごとに交互、全タスク完了)
#   シナリオ E: 'l' でファイル一覧 (hello.txt と uprog.elf)
#
# 使い方: ./test.sh    (このディレクトリで実行)
# 終了コード: すべて PASS なら 0、どこか FAIL なら 1

set -u
cd "$(dirname "$0")"

# QEMU の選択は build.sh と同じ優先順位
TOOLS_DIR=$(CDPATH= cd -- "../../../tools" 2>/dev/null && pwd || true)
if [ -z "${QEMU:-}" ] && [ -n "$TOOLS_DIR" ] && [ -x "$TOOLS_DIR/qemu/qemu-system-riscv64" ]; then
    QEMU="$TOOLS_DIR/qemu/qemu-system-riscv64"
fi
QEMU=${QEMU:-qemu-system-riscv64}

echo "== build == (takes a few seconds; QEMU runs make Ctrl-C"
echo "   ineffective while they run -- Ctrl-A x ends that QEMU)"
if ! (cd ../.. && BUILD_NO_RUN=1 ./build.sh) > build-test.log 2>&1; then
    echo "FAIL: build (see build-test.log)"
    exit 1
fi
echo "build ok (QEMU=$QEMU)"

fails=0

# 順序どおりチェック: $1 = 出力、残りの引数 = 期待する部分文字列
check_order() {
    out=$1; shift
    for want in "$@"; do
        case "$out" in
            *"$want"*) out=${out#*"$want"} ;;
            *)
                echo "  missing (in order): $want"
                return 1
                ;;
        esac
    done
    return 0
}

report() {
    if [ "$2" -eq 0 ]; then
        echo "PASS: $1"
    else
        echo "FAIL: $1"
        fails=$((fails + 1))
    fi
}

# tick の割り込み表示を取り除く
strip_ticks() {
    tr -d '\000' | sed -E 's/tick [0-9]+//g'
}

echo "== scenario A: boot =="
# 起動直後は Nish の入力待ち (割り込み不可の getc) で tick が
# 止まるので、exit でカーネルコンソールに落ちてから tick を見る
{ sleep 3; printf 'exit\n'; sleep 3; } \
    | timeout 9 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-a.log 2>&1 || true
raw=$(cat test-a.log)
out=$(strip_ticks < test-a.log)
case "$raw" in *"tick 1"*) tick_ok=0 ;; *) tick_ok=1 ;; esac
check_order "$out" "vm: Sv39 paging on" "hello from KosabaOS (S-mode)"
order_ok=$?
[ "$tick_ok" -eq 0 ] && [ "$order_ok" -eq 0 ]
report "A boot + paging + tick" $?

echo "== scenario B: kernel echo + page fault demo =="
{ sleep 4; printf 'exit\n'; sleep 2; printf 'abc'; sleep 2; printf 'x'; sleep 3; } \
    | timeout 14 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-b.log 2>&1 || true
out=$(strip_ticks < test-b.log)
check_order "$out" "hello from KosabaOS (S-mode)" "abc" \
    "vm: touching unmapped address 0x60000000" \
    "page fault (code=13)" "stval=0x0000000060000000"
report "B echo + fault demo" $?

echo "== scenario C: user program =="
{ sleep 4; printf 'exit\n'; sleep 2; printf 'u'; sleep 3; printf 'z'; sleep 2; printf 'q'; sleep 2; printf 'w'; sleep 2; } \
    | timeout 14 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-c.log 2>&1 || true
out=$(strip_ticks < test-c.log)
check_order "$out" "hello from KosabaOS (S-mode)" \
    "[kernel] starting uprog.elf" "hello from user mode!" \
    "data probe: 8 x" "z" \
    "[kernel] back from user program" "w"
report "C user mode round trip" $?

echo "== scenario D: multitasking demo =="
{ sleep 4; printf 'exit\n'; sleep 2; printf 'm'; sleep 16; } \
    | timeout 22 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-d.log 2>&1 || true
out=$(strip_ticks < test-d.log)
check_order "$out" "[proc] cooperative" "ABABAB" \
    "[proc] preemptive" "AAAAAAAA" "BBBBBBBB" \
    "[proc] all tasks done"
report "D cooperative + preemptive multitasking" $?

echo "== scenario E: filesystem listing =="
{ sleep 4; printf 'exit\n'; sleep 2; printf 'l'; sleep 2; } \
    | timeout 8 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-e.log 2>&1 || true
out=$(strip_ticks < test-e.log)
check_order "$out" "[fs] files:" "hello.txt" "uprog.elf"
report "E filesystem listing" $?

echo "== scenario F: shell =="
{ sleep 3; printf 'ls\n'; sleep 2; \
  printf 'cat hello.txt\n'; sleep 2; printf 'run uprog.elf\n'; sleep 3; \
  printf 'q'; sleep 2; printf 'exit\n'; sleep 2; } \
    | timeout 20 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-f.log 2>&1 || true
out=$(strip_ticks < test-f.log)
check_order "$out" "Nish, the KosabaOS shell" "nish> " "nish.elf" \
    "Hello from the KosabaOS RAM filesystem." "hello from user mode!" \
    "nish> " "[kernel] back from user program"
report "F shell (ls / cat / run / exit)" $?

echo "== scenario G: shell write =="
{ sleep 3; printf 'write memo.txt hello-write-test\n'; sleep 2; \
  printf 'ls\n'; sleep 2; printf 'cat memo.txt\n'; sleep 2; \
  printf 'exit\n'; sleep 2; } \
    | timeout 18 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-g.log 2>&1 || true
out=$(strip_ticks < test-g.log)
check_order "$out" "Nish, the KosabaOS shell" "wrote memo.txt" "memo.txt" \
    "hello-write-test" "[kernel] back from user program"
report "G shell write (create / write / cat)" $?

echo "== scenario H: Nish line editing + history =="
{ sleep 3; printf 'lx\x7fs\n'; sleep 2; \
  printf 'cat hello.txt\n'; sleep 2; printf '\033[A\n'; sleep 2; \
  printf 'lxs\033[D\x7f\n'; sleep 2; printf 'exit\n'; sleep 1; } \
    | timeout 18 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-h.log 2>&1 || true
out=$(strip_ticks < test-h.log)
cats=$(printf '%s' "$out" | grep -o 'Hello from the KosabaOS RAM filesystem\.' | wc -l)
if [ "$cats" -ge 2 ] && printf '%s' "$out" | grep -q 'nish.elf'; then rc=0; else rc=1; fi
report "H line editing (BS / arrows) + history recall" $rc

echo "== scenario I: shell run failure =="
{ sleep 3; printf 'run nosuch.elf\n'; sleep 2; \
  printf 'run hello.txt\n'; sleep 2; printf 'ls\n'; sleep 2; \
  printf 'exit\n'; sleep 2; } \
    | timeout 18 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-i.log 2>&1 || true
out=$(strip_ticks < test-i.log)
check_order "$out" "Nish, the KosabaOS shell" "run: cannot start" \
    "run: cannot start" "nish.elf" "[kernel] back from user program"
report "I shell run failure (missing / not an ELF)" $?

echo "== scenario J: exec with arguments =="
{ sleep 3; printf 'run uprog.elf aa bb cc\n'; sleep 2; \
  printf 'q'; sleep 2; printf 'exit\n'; sleep 2; } \
    | timeout 18 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-j.log 2>&1 || true
out=$(strip_ticks < test-j.log)
check_order "$out" "Nish, the KosabaOS shell" "hello from user mode!" \
    "args: aa bb cc" "[kernel] back from user program"
report "J exec args (run uprog.elf aa bb cc)" $?

echo "== scenario K: exit status =="
{ sleep 3; printf 'run uprog.elf\n'; sleep 2; \
  printf 'e'; sleep 2; printf 'exit\n'; sleep 2; } \
    | timeout 18 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-k.log 2>&1 || true
out=$(strip_ticks < test-k.log)
check_order "$out" "Nish, the KosabaOS shell" "hello from user mode!" \
    "run: exit status 7" "[kernel] back from user program"
report "K exit status (uprog exits with 7 via 'e')" $?

echo "== scenario L: SD card (FAT32 on ramdisk) =="
{ sleep 3; printf 'cat /sd/HELLO.TXT\n'; sleep 3; \
  printf 'cat /sd/SUB/INNER.TXT\n'; sleep 2; \
  printf 'cat /sd/NOPE.TXT\n'; sleep 2; \
  printf 'exit\n'; sleep 2; } \
    | timeout 25 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-l.log 2>&1 || true
out=$(strip_ticks < test-l.log)
check_order "$out" "Nish, the KosabaOS shell" "SD line 0000" \
    "SD-END-MARKER" "SD-INNER-MARKER" "cat: no such file" \
    "[kernel] back from user program"
report "L SD card (FAT32: root file, subdir file, missing file)" $?

echo "== scenario M: fs stage 2 (64 files + rm) =="
{ sleep 3; \
  for i in 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17; do \
    printf "write f$i.txt x\n"; sleep 0.4; \
  done; \
  printf 'ls\n'; sleep 2; \
  printf 'rm f01.txt\n'; sleep 1; \
  printf 'cat f01.txt\n'; sleep 1; \
  printf 'exit\n'; sleep 2; } \
    | timeout 40 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-m.log 2>&1 || true
out=$(strip_ticks < test-m.log)
check_order "$out" "Nish, the KosabaOS shell" "f17.txt" \
    "removed f01.txt" "cat: no such file" "[kernel] back from user program"
report "M fs stage 2 (create 17 files, rm, cat fails)" $?

echo "== scenario N: page allocator demo =="
{ sleep 3; printf 'exit\n'; sleep 2; printf 'p'; sleep 2; } \
    | timeout 15 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-n.log 2>&1 || true
out=$(strip_ticks < test-n.log)
check_order "$out" "[kernel] back from user program" \
    "page: pool total 24064, free 24064" \
    "page: alloc 0x0000000082200000" "(reused)" "(back to start)"
report "N page allocator (alloc / free / reuse, no leak)" $?

echo "== scenario O: fs stage 4 (directories + cwd) =="
{ sleep 3; printf 'mkdir work\n'; sleep 1; \
  printf 'write work/memo.txt dir-test\n'; sleep 1; \
  printf 'ls work\n'; sleep 1; \
  printf 'cd work\n'; sleep 1; \
  printf 'pwd\n'; sleep 1; \
  printf 'cat memo.txt\n'; sleep 1; \
  printf 'cat ../hello.txt\n'; sleep 1; \
  printf 'cd ..\n'; sleep 1; \
  printf 'ls\n'; sleep 1; \
  printf 'ls /sd\n'; sleep 2; \
  printf 'ls /sd/SUB\n'; sleep 1; \
  printf 'rmdir work\n'; sleep 1; \
  printf 'rm work/memo.txt\n'; sleep 1; \
  printf 'rmdir work\n'; sleep 1; \
  printf 'cd work\n'; sleep 1; \
  printf 'exit\n'; sleep 2; } \
    | timeout 35 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-o.log 2>&1 || true
out=$(strip_ticks < test-o.log)
check_order "$out" "Nish, the KosabaOS shell" "created work" \
    "wrote work/memo.txt" "memo.txt" "/work" "dir-test" \
    "Hello from the KosabaOS RAM filesystem." "work/" \
    "HELLO.TXT" "SUB/" "INNER.TXT" "rmdir: cannot remove" \
    "removed work/memo.txt" "removed work" \
    "cd: no such directory" "[kernel] back from user program"
report "O fs stage 4 (mkdir/cd/pwd, relative paths, ls /sd)" $?

echo "== scenario P: exec multilevel (chain) =="
# chain.elf が自分を 3 段まで exec し、終了ステータスが 1 段
# ごとに +1 されて戻ること。一番上は fd を開いたまま子へ行き、
# 戻った後に続きが読めること (親の fd とメモリの生存)。
# スタックは再帰で初期分を超えて使い、fault 伸長も見る。
# deep 25 は sanity 上限 (16) で exec が -1 になり、底には
# 届かないこと ("deep bottom" が出ないことも確認する)
{ sleep 3; printf 'run chain.elf 3\n'; sleep 3; \
  printf 'run chain.elf deep 25\n'; sleep 3; \
  printf 'exit\n'; sleep 2; } \
    | timeout 20 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-p.log 2>&1 || true
out=$(strip_ticks < test-p.log)
rc=0
check_order "$out" "Nish, the KosabaOS shell" \
    "chain 3 start" "burn=4160" "chain 2 start" "chain 1 start" \
    "chain 0 start" "chain 1 child=3" "chain 2 child=4" \
    "chain 3 child=5" "fdok= fro" "mem=Hello" \
    "run: exit status 6" "deep denied" "run: exit status 9" \
    "[kernel] back from user program" || rc=1
case "$out" in
    *"deep bottom"*) echo "  unexpected: deep bottom (sanity limit not enforced)"; rc=1 ;;
esac
report "P exec multilevel (chain: depth / status / fd survival / limit)" $rc

echo "== scenario Q: kernel log (dmesg) =="
# Nish の dmesg で起動時の記録 (プールの総数など) が読み
# 返せることと、chain の deep 25 (深さ上限の拒否) のあとに
# 拒否の理由がログに残っていること。カーネルのメッセージは
# 起きた瞬間にコンソールにも出るので、2 回目の dmesg では
# 「run: exit status 9 よりあとに、もう一度その行が出る」
# ことで読み返しを確認する (check_order は順序どおりの
# 出現を見るので、2 回目の出現に当たる)
{ sleep 3; printf 'dmesg\n'; sleep 2; \
  printf 'run chain.elf deep 25\n'; sleep 3; \
  printf 'dmesg\n'; sleep 3; \
  printf 'exit\n'; sleep 2; } \
    | timeout 20 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-q.log 2>&1 || true
out=$(strip_ticks < test-q.log)
rc=0
check_order "$out" "Nish, the KosabaOS shell" \
    "page: pool total 24064, free 24064" \
    "deep denied" "run: exit status 9" \
    "exec: denied, depth limit: chain.elf" || rc=1
case "$out" in
    *"deep bottom"*) echo "  unexpected: deep bottom (sanity limit not enforced)"; rc=1 ;;
esac
report "Q kernel log (dmesg: boot records + exec denial)" $rc

echo "== scenario R: uptime syscall =="
# uptime.elf が syscall 31 (起動以来のミリ秒) を呼んで、
# (1) 2 回の読みが単調 (減らない)、(2) 500ms の待ちで値が
# その分進む、(3) 起動直後の値が大まかな範囲 (1 時間未満)
# に収まる、ことをプログラム側で判定して "uptime ok" と
# 出す。QEMU の時間は揺れるので絶対値の一致は見ず、
# マーカーの順序と "uptime FAIL" の不在で判定する
{ sleep 3; printf 'run uptime.elf\n'; sleep 4; \
  printf 'exit\n'; sleep 2; } \
    | timeout 15 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-r.log 2>&1 || true
out=$(strip_ticks < test-r.log)
rc=0
check_order "$out" "Nish, the KosabaOS shell" \
    "uptime t0=" "uptime t1=" "uptime ok" \
    "[kernel] back from user program" || rc=1
case "$out" in
    *"uptime FAIL"*) echo "  unexpected: uptime FAIL"; rc=1 ;;
esac
report "R uptime (monotonic / advances / sane range)" $rc

echo "== scenario S: wall clock (Goldfish RTC) =="
# カーネルコンソールの 't' キーで壁時計を読む (doc/time.md)。
# Goldfish RTC はホストの時刻 (UTC) を返すので、epoch が
# ホストの現在時刻と大きくずれていないこと、2 回読むと
# 進んでいること、設定は read-only で断られることを見る
host_epoch=$(date -u +%s)
{ sleep 3; printf 'exit\n'; sleep 2; printf 't'; sleep 1; \
  printf '\n'; sleep 3; printf 't'; sleep 1; \
  printf '2031-02-03 04:05:06\n'; sleep 2; } \
    | timeout 20 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-s.log 2>&1 || true
out=$(strip_ticks < test-s.log)
rc=0
# epoch の抽出は check_order より先 (check_order は out を
# 消費してしまうため)
epochs=$(printf '%s' "$out" | grep -o 'epoch [0-9][0-9]*' | awk '{print $2}')
check_order "$out" "time: wall clock 20" "wall: 20" \
    "time: kept" "time: cannot set on this board" || rc=1
e1=$(printf '%s\n' "$epochs" | sed -n 1p)
e2=$(printf '%s\n' "$epochs" | sed -n 2p)
if [ -z "$e1" ] || [ -z "$e2" ]; then
    echo "  missing epoch values (e1=$e1 e2=$e2)"
    rc=1
fi
if [ "$rc" -eq 0 ]; then
    if [ "$e2" -le "$e1" ]; then
        echo "  epoch not advancing: $e1 -> $e2"
        rc=1
    fi
    d=$((e1 - host_epoch))
    [ "$d" -lt 0 ] && d=$((-d))
    if [ "$d" -gt 300 ]; then
        echo "  epoch off host by ${d}s: $e1 vs host $host_epoch"
        rc=1
    fi
fi
report "S wall clock (Goldfish RTC: read / advance / read-only)" $rc

echo "== scenario T: audio syscalls (dummy device) + FP =="
# atest.elf が audio_open(44100, 1) → 既知の列を分けて
# audio_write (300 + 300 + 400 = 1000 サンプル) → audio_close
# し、Nish の dmesg に出る "audio: close" 行の samples と
# fnv がホスト側の期待値と一致すること。開く前の write /
# close、変なチャンネル数、再 open、範囲外ポインタなどの
# -1 系は atest 側が確かめて "atest: ... ok" を出す
# (カーネルのメッセージは起きた瞬間にコンソールにも出る
# ので、"audio: close" 行は atest: done より後 = dmesg での
# 読み返しに当たる出現で照合する)
# あわせて fptest.elf (lp64d) で、ユーザープログラム開始時に
# FPU が有効化されており float / double の四則と変換が
# 実行時の命令として正しく動くことを確認する
expected_fnv=$(python3 -c '
h = 0xcbf29ce484222325
for i in range(1000):
    v = (i * 37 + 11) & 0xFFFF
    for b in (v & 0xFF, v >> 8):
        h ^= b
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
print("%016x" % h)')
{ sleep 3; printf 'run atest.elf\n'; sleep 3; \
  printf 'run fptest.elf\n'; sleep 2; \
  printf 'dmesg\n'; sleep 3; \
  printf 'exit\n'; sleep 2; } \
    | timeout 22 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-t.log 2>&1 || true
out=$(strip_ticks < test-t.log)
rc=0
check_order "$out" "Nish, the KosabaOS shell" \
    "atest: errors ok" "atest: open ok" "atest: writes ok" \
    "atest: close ok" "atest: done" \
    "fptest: float ok" "fptest: double ok" "fptest: conv ok" \
    "fptest: done" \
    "audio: close rate=44100 channels=1 samples=1000 fnv=$expected_fnv" || rc=1
case "$out" in
    *"atest: FAIL"*) echo "  unexpected: atest reported FAIL"; rc=1 ;;
    *"fptest: FAIL"*) echo "  unexpected: fptest reported FAIL"; rc=1 ;;
esac
report "T audio syscalls (fnv via dmesg) + FP arithmetic" $rc

echo "== summary =="
if [ "$fails" -eq 0 ]; then
    echo "ALL PASS"
else
    echo "$fails scenario(s) failed (logs: test-a.log test-b.log test-c.log)"
fi
exit "$fails"
