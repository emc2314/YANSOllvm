; RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %s -o %t.out.ll
; RUN: grep '__yansollvm_mfla_main' %t.out.ll
; RUN: grep 'call i32 @rec' %t.out.ll
; RUN: grep 'call void @__yansollvm_mfla_main' %t.out.ll
; RUN: %not grep 'recursive SCC not supported' %t.out.ll

; Recursive callees are transformable now. A caller that invokes one should
; still be transformable; recursive calls are left as native wrapper calls in the
; cloned mega body rather than being CPS-lowered through singleton ret-cont slots.

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
