; RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %s -o %t.out.ll 2>&1 | %FileCheck %s
; RUN: grep '__yansollvm_mfla_main' %t.out.ll
; RUN: grep 'call i32 @rec' %t.out.ll
; RUN: grep 'call void @__yansollvm_mfla_main' %t.out.ll

; CHECK: yansollvm: warning: mfla: skip function 'rec': recursive SCC not supported

; A recursive callee is skipped. A non-recursive caller that invokes it should
; still be transformable: the call to @rec must remain a native call in the
; cloned mega body rather than being treated as an internal CPS-lowered call.

define internal i32 @rec(i32 %x) nounwind {
entry:
  %done = icmp eq i32 %x, 0
  br i1 %done, label %base, label %recur
base:
  ret i32 1
recur:
  %dec = add i32 %x, -1
  %rv = call i32 @rec(i32 %dec)
  %sum = add i32 %rv, 1
  ret i32 %sum
}

define internal i32 @caller(i32 %x) nounwind {
entry:
  %rv = call i32 @rec(i32 %x)
  %sum = add i32 %rv, 7
  ret i32 %sum
}

define internal i32 @sibling(i32 %x) nounwind {
entry:
  %y = add i32 %x, 3
  ret i32 %y
}
