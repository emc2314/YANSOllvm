; RUN: %opt -load-pass-plugin %plugin -passes=merge,verify -merge-max-group-size=2 -S %s -o %t.out.ll
; RUN: %FileCheck %s --input-file=%t.out.ll

; CHECK-LABEL: define i32 @pub_a(i32 %x)
; CHECK: call i64 @pub_a.main.merge
; CHECK-LABEL: define i32 @pub_b(i32 %x)
; CHECK-NOT: call i64 @pub_a.main.merge
; CHECK-LABEL: define i32 @main(i32 %x)
; CHECK: call i64 @pub_a.main.merge
; CHECK-NOT: call i32 @pub_a
; CHECK-LABEL: define internal i64 @pub_a.main.merge(i64 %0, i32 %1)
; CHECK: add i32 %1, 11
; CHECK: call i32 @pub_b

define i32 @pub_a(i32 %x) {
entry:
  %r = add i32 %x, 11
  ret i32 %r
}

define i32 @pub_b(i32 %x) {
entry:
  %r = mul i32 %x, 7
  ret i32 %r
}

define i32 @main(i32 %x) {
entry:
  %a = call i32 @pub_a(i32 %x)
  %b = call i32 @pub_b(i32 %a)
  ret i32 %b
}
