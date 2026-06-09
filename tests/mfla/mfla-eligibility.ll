; RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %s -o %t.out.ll 2>&1 | %FileCheck %s
; RUN: grep '__yansollvm_mfla_main' %t.out.ll
; RUN: grep 'indirectbr' %t.out.ll
; RUN: grep 'blockaddress(@__yansollvm_mfla_main' %t.out.ll
; RUN: grep -qv '@vararg(' %t.out.ll

; CHECK: yansollvm: warning: mfla: skip function 'vararg': vararg function
; CHECK: yansollvm: error: mfla: skip function 'has_indirectbr': contains indirectbr/callbr
; CHECK: yansollvm: warning: mfla: skip function 'has_blockaddress': contains blockaddress constant
; CHECK: yansollvm: warning: mfla: skip function 'self_rec': recursive SCC not supported
; CHECK: yansollvm: warning: mfla: skip function 'mut_a': recursive SCC not supported
; CHECK: yansollvm: warning: mfla: skip function 'mut_b': recursive SCC not supported

define internal i32 @add1(i32 %x) {
entry:
  %y = add i32 %x, 1
  ret i32 %y
}

define internal i32 @add2(i32 %x) {
entry:
  %y = add i32 %x, 2
  ret i32 %y
}

define internal i32 @vararg(i32 %x, ...) {
entry:
  ret i32 %x
}

define internal void @has_indirectbr(ptr %p) {
entry:
  indirectbr ptr %p, [label %target]
target:
  ret void
}

define internal ptr @has_blockaddress() {
entry:
  ret ptr blockaddress(@has_blockaddress, %target)
target:
  ret ptr null
}

define internal i32 @self_rec(i32 %x) {
entry:
  %done = icmp eq i32 %x, 0
  br i1 %done, label %ret, label %recur
recur:
  %dec = add i32 %x, -1
  %y = call i32 @self_rec(i32 %dec)
  ret i32 %y
ret:
  ret i32 0
}

define internal i32 @mut_a(i32 %x) {
entry:
  %done = icmp eq i32 %x, 0
  br i1 %done, label %ret, label %call
call:
  %dec = add i32 %x, -1
  %y = call i32 @mut_b(i32 %dec)
  ret i32 %y
ret:
  ret i32 0
}

define internal i32 @mut_b(i32 %x) {
entry:
  %done = icmp eq i32 %x, 0
  br i1 %done, label %ret, label %call
call:
  %dec = add i32 %x, -1
  %y = call i32 @mut_a(i32 %dec)
  ret i32 %y
ret:
  ret i32 0
}
