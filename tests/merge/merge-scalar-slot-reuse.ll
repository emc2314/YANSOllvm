; RUN: %opt -load-pass-plugin %plugin -passes=merge,verify -merge-max-group-size=2 -S %s -o %t.out.ll
; RUN: %FileCheck %s --input-file=%t.out.ll

; CHECK-DAG: define internal i64 @fi.di.merge(i64 %0, i64 %1, i64 %2)
; CHECK-DAG: bitcast float %f to i32
; CHECK-DAG: zext i32 {{.*}} to i64
; CHECK-DAG: trunc i64 {{.*}} to i32
; CHECK-DAG: bitcast i32 {{.*}} to float
; CHECK-DAG: bitcast i64 {{.*}} to double

define internal i32 @fi(i32 %x, float %f) {
entry:
  %fb = bitcast float %f to i32
  %r = add i32 %x, %fb
  ret i32 %r
}

define internal i64 @di(i64 %x, double %d) {
entry:
  %db = bitcast double %d to i64
  %r = xor i64 %x, %db
  ret i64 %r
}

define i64 @caller(i32 %x, i64 %y, float %f, double %d) {
entry:
  %a = call i32 @fi(i32 %x, float %f)
  %b = call i64 @di(i64 %y, double %d)
  %a64 = zext i32 %a to i64
  %r = add i64 %a64, %b
  ret i64 %r
}
