#ifndef KOSABAOS_PROC_H
#define KOSABAOS_PROC_H

#include <stdint.h>

/* 実行の文脈 (switch.S と並びを一致させること) */
struct context {
    uint64_t ra;
    uint64_t sp;
    uint64_t s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
};

void proc_demo(void);   /* 'm' キーで起動するマルチタスクのデモ */
int proc_list(char *buf, unsigned long max);  /* タスク一覧 (ps 用) */

/* cwd (カレントディレクトリ)。タスクごとに、正規化済みの
 * 絶対パス文字列で持つ (FS 再設計 段4)。ユーザープログラムは
 * タスク 0 (kmain の文脈) で動くので、シェルの cd がここに
 * 効き、exec では trap.c が親の分を退避・復元する */
#define PROC_CWD_MAX 128
const char *proc_cwd(void);             /* 現在タスクの cwd */
void proc_set_cwd(const char *abs);     /* 正規化済みの絶対パスを設定 */

/* スケジューラ (proc.c)。trap.c のタイマ処理からも使う */
extern int sched_active;
extern int preempt_enabled;
void schedule(void);
void yield(void);

/* トラップをまたぐスケジューリングでは、CSR の sepc / sstatus は
 * CPU に 1 組しかないので、タスクごとに退避・復元が要る */
void sched_save_trapstate(uint64_t sepc, uint64_t sstatus);
void sched_restore_trapstate(uint64_t *sepc, uint64_t *sstatus);

#endif
