; RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %s -o %t.out.ll 2>&1 | %FileCheck %s
; RUN: grep -qv '__yansollvm_mfla_main' %t.out.ll
; RUN: grep -qv '__yansollvm_mfla_frame' %t.out.ll

; CHECK: yansollvm: warning: mfla: skip module '{{.*}}': fewer than two eligible functions

define internal i32 @only_one(i32 %x) {
entry:
  %y = add i32 %x, 1
  ret i32 %y
}
