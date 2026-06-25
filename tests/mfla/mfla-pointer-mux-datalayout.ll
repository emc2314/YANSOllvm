; RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %s -o %t.out.ll
; RUN: grep 'define internal void @__yansollvm_mfla_main(ptr' %t.out.ll
; RUN: grep '@__yansollvm_mfla_edge = private unnamed_addr constant i64' %t.out.ll
; RUN: grep 'ptrtoint (ptr blockaddress.* to i64' %t.out.ll
; RUN: grep 'indirectbr ptr %mfla.target' %t.out.ll

target datalayout = "e-p:32:32"

define internal i32 @choose(i1 %cond, i32 %x) nounwind {
entry:
  br i1 %cond, label %t, label %f
t:
  %a = add i32 %x, 1
  ret i32 %a
f:
  %b = add i32 %x, 2
  ret i32 %b
}

define internal i32 @other(i32 %x) nounwind {
entry:
  %y = add i32 %x, 3
  ret i32 %y
}
