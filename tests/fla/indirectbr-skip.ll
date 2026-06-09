; RUN: %opt -load-pass-plugin %plugin -passes=fla,verify -S %s -o %t 2>&1 | grep 'contains indirectbr'
; RUN: grep 'indirectbr' %t

@main.L = internal global [2 x ptr] [ptr blockaddress(@main, %L1), ptr blockaddress(@main, %L2)], align 16

define i32 @main() {
entry:
  br label %L1

L1:
  %dest = load ptr, ptr @main.L, align 8
  indirectbr ptr %dest, [label %L1, label %L2]

L2:
  ret i32 0
}
