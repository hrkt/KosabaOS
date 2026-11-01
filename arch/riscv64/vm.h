#ifndef KOSABAOS_VM_H
#define KOSABAOS_VM_H

/* 仮想メモリ (Sv39 ページング)。実装は vm.c。 */

void vm_init(void);        /* ページテーブルを作り、変換を有効にする */
void vm_fault_demo(void);  /* わざと未マップ住所を触ってフォルトさせる */

#endif
