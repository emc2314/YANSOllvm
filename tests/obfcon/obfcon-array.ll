; RUN: %opt -load-pass-plugin %plugin -passes=obfcon -verify-each -S %s -o %t
; RUN: %FileCheck %s < %t
; RUN: %lli %t

; CHECK-NOT: @a8 =
; CHECK: @a8.obf.a = private unnamed_addr constant
; CHECK: @a8.obf.b = private unnamed_addr constant
; CHECK: @a16.obf.a = private unnamed_addr constant
; CHECK: @a32.obf.a = private unnamed_addr constant
; CHECK: @a64.obf.a = private unnamed_addr constant
; CHECK: obfcon.arr.load.a
; CHECK: obfcon.arr.load.b

@a8 = private constant [4 x i8] [i8 3, i8 17, i8 99, i8 -55], align 1
@a16 = internal constant [4 x i16] [i16 7, i16 4660, i16 -25033, i16 -36], align 2
@a32 = private constant [4 x i32] [i32 11, i32 305419896, i32 -1640531527, i32 -559038737], align 4
@a64 = private constant [4 x i64] [i64 13, i64 1311768467463790320, i64 7640891576956012808, i64 -81985529216486896], align 8

define i64 @get(i64 %index) {
entry:
  %i = and i64 %index, 3
  %p8 = getelementptr inbounds [4 x i8], ptr @a8, i64 0, i64 %i
  %v8 = load i8, ptr %p8, align 1
  %e8 = zext i8 %v8 to i64
  %p16 = getelementptr inbounds [4 x i16], ptr @a16, i64 0, i64 %i
  %v16 = load i16, ptr %p16, align 2
  %e16 = zext i16 %v16 to i64
  %p32 = getelementptr inbounds [4 x i32], ptr @a32, i64 0, i64 %i
  %v32 = load i32, ptr %p32, align 4
  %e32 = zext i32 %v32 to i64
  %p64 = getelementptr inbounds [4 x i64], ptr @a64, i64 0, i64 %i
  %v64 = load i64, ptr %p64, align 8
  %r0 = add i64 %e8, %e16
  %r1 = add i64 %r0, %e32
  %r2 = xor i64 %r1, %v64
  ret i64 %r2
}

define i32 @main() {
entry:
  %value = call i64 @get(i64 2)
  %ok = icmp eq i64 %value, 7640891574704197979
  %status = select i1 %ok, i32 0, i32 1
  ret i32 %status
}
