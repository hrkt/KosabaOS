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
timeout 8 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    < /dev/null > test-a.log 2>&1 || true
raw=$(cat test-a.log)
out=$(strip_ticks < test-a.log)
case "$raw" in *"tick 1"*) tick_ok=0 ;; *) tick_ok=1 ;; esac
check_order "$out" "vm: Sv39 paging on" "hello from KosabaOS (S-mode)"
order_ok=$?
[ "$tick_ok" -eq 0 ] && [ "$order_ok" -eq 0 ]
report "A boot + paging + tick" $?

echo "== scenario B: kernel echo + page fault demo =="
{ sleep 3; printf 'abc'; sleep 2; printf 'x'; sleep 3; } \
    | timeout 9 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-b.log 2>&1 || true
out=$(strip_ticks < test-b.log)
check_order "$out" "hello from KosabaOS (S-mode)" "abc" \
    "vm: touching unmapped address 0x60000000" \
    "page fault (code=13)" "stval=0x0000000060000000"
report "B echo + fault demo" $?

echo "== scenario C: user program =="
{ sleep 3; printf 'u'; sleep 3; printf 'z'; sleep 2; printf 'q'; sleep 2; printf 'w'; sleep 2; } \
    | timeout 14 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-c.log 2>&1 || true
out=$(strip_ticks < test-c.log)
check_order "$out" "hello from KosabaOS (S-mode)" \
    "[kernel] starting user program" "hello from user mode!" "z" \
    "[kernel] back from user program" "w"
report "C user mode round trip" $?

echo "== scenario D: multitasking demo =="
{ sleep 3; printf 'm'; sleep 16; } \
    | timeout 22 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-d.log 2>&1 || true
out=$(strip_ticks < test-d.log)
check_order "$out" "[proc] cooperative" "ABABAB" \
    "[proc] preemptive" "AAAAAAAA" "BBBBBBBB" \
    "[proc] all tasks done"
report "D cooperative + preemptive multitasking" $?

echo "== scenario E: filesystem listing =="
{ sleep 3; printf 'l'; sleep 2; } \
    | timeout 8 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic \
    > test-e.log 2>&1 || true
out=$(strip_ticks < test-e.log)
check_order "$out" "[fs] files:" "hello.txt" "uprog.elf"
report "E filesystem listing" $?

echo "== summary =="
if [ "$fails" -eq 0 ]; then
    echo "ALL PASS"
else
    echo "$fails scenario(s) failed (logs: test-a.log test-b.log test-c.log)"
fi
exit "$fails"
