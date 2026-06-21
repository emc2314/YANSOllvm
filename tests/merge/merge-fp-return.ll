; RUN: %opt -load-pass-plugin %plugin -passes=merge,verify -merge-max-group-size=2 -S %s -o %t.out.ll
; RUN: %FileCheck %s --input-file=%t.out.ll
; RUN: %clang %t.out.ll -o %t.exe
; RUN: %t.exe

; CHECK-DAG: define internal i64 @ff.main.merge(i64 %0, i32 %1)
; CHECK-DAG: zext i32 {{.*}} to i64
; CHECK-DAG: trunc i64 {{.*}} to i32
; CHECK-DAG: bitcast i32 {{.*}} to float

define internal float @ff(float %x) {
entry:
  %r = fadd float %x, 1.500000e+00
  ret float %r
}

define internal double @dd(double %x) {
entry:
  %r = fmul double %x, 2.500000e+00
  ret double %r
}

define i32 @main() {
entry:
  %a = call float @ff(float 1.250000e+00)
  %b = call double @dd(double 3.500000e+00)
  %a64 = fpext float %a to double
  %r = fadd double %a64, %b
  %ri = fptosi double %r to i32
  %ok = icmp eq i32 %ri, 11
  %exit = select i1 %ok, i32 0, i32 1
  ret i32 %exit
}
