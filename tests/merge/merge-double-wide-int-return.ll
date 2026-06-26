; RUN: %opt -load-pass-plugin %plugin -passes=merge,verify -merge-max-group-size=8 -S %s -o %t.out.ll
; RUN: %clang %t.out.ll -o %t.exe
; RUN: %t.exe

; Regression test for MergePass return-value coercion.
;
; When a merge group mixes a `double`-returning member with an integer member
; wider than 64 bits, the group's merged return width is > 64.  The `double`
; coercion path must widen its i64 bitcast up to the merged width (and narrow
; back on the way out), exactly as the `float` path does.  Bitcasting `double`
; straight to i64 and returning it from a wider dispatcher is an illegal ZExt
; that aborts the compiler (and an invalid `ret` even if it did not).

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define internal i128 @ret_wide(i64 %x) noinline {
  %z = zext i64 %x to i128
  %r = add i128 %z, 1
  ret i128 %r
}

define internal double @ret_double(i64 %x) noinline {
  %f = uitofp i64 %x to double
  %r = fadd double %f, 2.500000e+00
  ret double %r
}

define i32 @main() {
entry:
  %p = call i128 @ret_wide(i64 40)
  %d = call double @ret_double(i64 1)
  %pt = trunc i128 %p to i64           ; 41
  %di = fptosi double %d to i64        ; 3
  %sum = add i64 %pt, %di              ; 44
  %ok = icmp eq i64 %sum, 44
  %exit = select i1 %ok, i32 0, i32 1
  ret i32 %exit
}
