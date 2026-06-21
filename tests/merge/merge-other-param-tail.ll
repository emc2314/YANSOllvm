; RUN: %opt -load-pass-plugin %plugin -passes=merge,verify -merge-max-group-size=2 -S %s -o %t.out.ll
; RUN: %FileCheck %s --input-file=%t.out.ll
; RUN: %clang %t.out.ll -o %t.exe
; RUN: %t.exe

target datalayout = "e-m:e-p:64:64-i64:64-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%Padded = type { i8, i64, [3 x i16] }
%Nested = type { { i32, i8 }, double }
%VecWrap = type { <2 x i32>, i8 }
%Pair = type { i32, i32 }

; CHECK-DAG: define internal i64 @fd.main.merge(i64 %0, %Pair %1)
; CHECK-DAG: define internal i64 @fc.fa.merge(i64 %0, %VecWrap %1, %Padded %2)
; CHECK-DAG: call i64 @fc.fa.merge({{.*}}, %VecWrap zeroinitializer, %Padded { i8 5, i64 1234605616436508552, [3 x i16] [i16 7, i16 11, i16 13] })
; CHECK-DAG: call i64 @fc.fa.merge({{.*}}, %VecWrap { <2 x i32> <i32 37, i32 41>, i8 43 }, %Padded zeroinitializer)
; CHECK-DAG: call i64 @fd.main.merge({{.*}}, %Pair { i32 47, i32 53 })
; CHECK-DAG: call i32 @fb

define internal i32 @fa(%Padded %p) {
entry:
  %tag = extractvalue %Padded %p, 0
  %wide = extractvalue %Padded %p, 1
  %arr1 = extractvalue %Padded %p, 2, 1
  %tag32 = zext i8 %tag to i32
  %wide32 = trunc i64 %wide to i32
  %arr32 = zext i16 %arr1 to i32
  %x = xor i32 %wide32, %tag32
  %r = add i32 %x, %arr32
  ret i32 %r
}

define internal i32 @fb(%Nested %n, %VecWrap %v) {
entry:
  %a = extractvalue %Nested %n, 0, 0
  %b = extractvalue %Nested %n, 0, 1
  %d = extractvalue %Nested %n, 1
  %vec = extractvalue %VecWrap %v, 0
  %lane = extractelement <2 x i32> %vec, i32 1
  %tail = extractvalue %VecWrap %v, 1
  %b32 = zext i8 %b to i32
  %tail32 = zext i8 %tail to i32
  %di = fptosi double %d to i32
  %s0 = add i32 %a, %b32
  %s1 = add i32 %lane, %tail32
  %s2 = add i32 %s0, %di
  %r = add i32 %s2, %s1
  ret i32 %r
}

define internal i32 @fc(%VecWrap %v) {
entry:
  %vec = extractvalue %VecWrap %v, 0
  %lane0 = extractelement <2 x i32> %vec, i32 0
  %lane1 = extractelement <2 x i32> %vec, i32 1
  %tail = extractvalue %VecWrap %v, 1
  %tail32 = zext i8 %tail to i32
  %s = add i32 %lane0, %lane1
  %r = add i32 %s, %tail32
  ret i32 %r
}

define internal i32 @fd(%Pair %pair) {
entry:
  %x = extractvalue %Pair %pair, 0
  %y = extractvalue %Pair %pair, 1
  %r = sub i32 %y, %x
  ret i32 %r
}

define i32 @main() {
entry:
  %a = call i32 @fa(%Padded { i8 5, i64 1234605616436508552, [3 x i16] [i16 7, i16 11, i16 13] })
  %b = call i32 @fb(%Nested { { i32, i8 } { i32 17, i8 19 }, double 2.500000e+00 }, %VecWrap { <2 x i32> <i32 23, i32 29>, i8 31 })
  %c = call i32 @fc(%VecWrap { <2 x i32> <i32 37, i32 41>, i8 43 })
  %d = call i32 @fd(%Pair { i32 47, i32 53 })
  %ab = add i32 %a, %b
  %cd = add i32 %c, %d
  %r = add i32 %ab, %cd
  %ok = icmp eq i32 %r, 1432778873
  %exit = select i1 %ok, i32 0, i32 1
  ret i32 %exit
}
