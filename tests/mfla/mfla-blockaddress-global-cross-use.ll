; RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %s -o %t.ll
; RUN: grep '@edge.mfla' %t.ll
; RUN: %not grep '^@edge = .*inttoptr (i32 1 to ptr)' %t.ll
; RUN: %not grep 'load i64, ptr @edge,' %t.ll
; RUN: grep 'load i64, ptr @edge.mfla' %t.ll
; RUN: %clang %t.ll -o %t.exe
; RUN: %t.exe

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@state = internal global i64 0, align 8
@G = internal global i32 0, align 4
@llvm.compiler.used = appending global [4 x ptr] [ptr @caller, ptr @dispatch, ptr @dummy_a, ptr @dummy_b], section "llvm.metadata"

; The blockaddress global is semantically owned by @dispatch but used by @caller.
; MFLA must remap it at module scope, not only in @dispatch's per-function VMap.
@edge = private unnamed_addr constant i64 add (
  i64 sub (i64 ptrtoint (ptr blockaddress(@dispatch, %target) to i64),
           i64 ptrtoint (ptr blockaddress(@dispatch, %anchor) to i64)),
  i64 1234567), align 8

define internal i32 @caller() noinline nounwind {
entry:
  %edge = load i64, ptr @edge, align 8
  %v = call i32 @dispatch(i64 1234567, i64 %edge)
  ret i32 %v
}

define internal i32 @dispatch(i64 %state, i64 %edge) noinline nounwind {
entry:
  store i64 %state, ptr @state, align 8
  br label %anchor

anchor:
  %s = load i64, ptr @state, align 8
  %delta = sub i64 %edge, %s
  %addr = add i64 ptrtoint (ptr blockaddress(@dispatch, %anchor) to i64), %delta
  %ptr = inttoptr i64 %addr to ptr
  indirectbr ptr %ptr, [label %anchor, label %target]

target:
  store i32 99, ptr @G, align 4
  ret i32 99
}

define internal i32 @dummy_a(i32 %x) noinline nounwind {
entry:
  %y = add i32 %x, 1
  store i32 %y, ptr @G, align 4
  ret i32 %y
}

define internal i64 @dummy_b(i64 %x) noinline nounwind {
entry:
  %y = mul i64 %x, 2
  store i32 5, ptr @G, align 4
  ret i64 %y
}

define i32 @main() nounwind {
entry:
  %v = call i32 @caller()
  %ok = icmp eq i32 %v, 99
  %rc = select i1 %ok, i32 0, i32 1
  ret i32 %rc
}
