; RUN: rm -rf %t && mkdir -p %t
; RUN: %opt -load-pass-plugin %plugin -passes=bb2func,verify -S %s -o %t/bb2func.ll
; RUN: %FileCheck %s --input-file=%t/bb2func.ll
; RUN: %clang %t/bb2func.ll -o %t/bb2func
; RUN: %t/bb2func | grep '^switch-region:334$'

; CHECK-LABEL: define i32 @switch_region(
; CHECK: call {{.*}}@switch_region.
; CHECK: switch i16
; CHECK-LABEL: define internal {{.*}}@switch_region.
; CHECK: switch i32
; CHECK: exitStub

@.str = private unnamed_addr constant [18 x i8] c"switch-region:%d\0A\00", align 1

define i32 @switch_region(i32 %x) {
entry:
  switch i32 %x, label %def [
    i32 1, label %c1
    i32 2, label %c2
    i32 3, label %c3
    i32 4, label %c4
    i32 5, label %c5
  ]

c1:
  %a1 = add i32 %x, 10
  br label %merge

c2:
  %a2 = add i32 %x, 20
  br label %merge

c3:
  %a3 = add i32 %x, 30
  br label %merge

c4:
  %a4 = add i32 %x, 40
  br label %merge

c5:
  %a5 = add i32 %x, 50
  br label %merge

def:
  %ad = add i32 %x, 60
  br label %merge

merge:
  %r = phi i32 [ %a1, %c1 ], [ %a2, %c2 ], [ %a3, %c3 ], [ %a4, %c4 ],
               [ %a5, %c5 ], [ %ad, %def ]
  %out = mul i32 %r, 2
  ret i32 %out
}

declare i32 @printf(ptr, ...)

define i32 @main() {
entry:
  %a = call i32 @switch_region(i32 1)
  %b = call i32 @switch_region(i32 3)
  %ab = add i32 %a, %b
  %c = call i32 @switch_region(i32 5)
  %abc = add i32 %ab, %c
  %d = call i32 @switch_region(i32 8)
  %sum = add i32 %abc, %d
  call i32 (ptr, ...) @printf(ptr @.str, i32 %sum)
  ret i32 0
}
