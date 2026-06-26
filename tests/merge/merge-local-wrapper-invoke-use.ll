; RUN: %opt -load-pass-plugin %plugin -passes=merge,verify -merge-max-group-size=2 -S %s -o %t.out.ll
; RUN: %FileCheck %s --input-file=%t.out.ll

; CHECK-LABEL: define private i32 @foo(i32 %x)
; CHECK: call i64 @bar.main.merge
; CHECK-NOT: add i32 %x, 1
; CHECK-LABEL: define internal i64 @bar.main.merge
; CHECK: mul i32 %1, 3
; CHECK: call i32 @foo

define private i32 @foo(i32 %x) {
entry:
  %r = add i32 %x, 1
  ret i32 %r
}

define private i32 @bar(i32 %x) {
entry:
  %r = mul i32 %x, 3
  ret i32 %r
}

define i32 @main(i32 %x) personality ptr @__gxx_personality_v0 {
entry:
  %a = call i32 @foo(i32 %x)
  %b = invoke i32 @foo(i32 %a) to label %ok unwind label %lp
ok:
  %c = call i32 @bar(i32 %b)
  ret i32 %c
lp:
  %l = landingpad { ptr, i32 } cleanup
  ret i32 -1
}

declare i32 @__gxx_personality_v0(...)
