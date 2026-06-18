; RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %s -o %t.out.ll 2>&1 | %FileCheck %s
; RUN: grep '__yansollvm_mfla_main' %t.out.ll
; RUN: grep 'define internal i32 @mixed_candidate' %t.out.ll
; RUN: grep 'define internal i32 @shared_candidate' %t.out.ll
; RUN: grep 'load ptr, ptr @shared_tab' %t.out.ll
; RUN: %not grep 'load ptr, ptr @shared_tab.mfla' %t.out.ll
; RUN: grep 'call void @__yansollvm_mfla_main' %t.out.ll
; RUN: %clang %t.out.ll -o %t.exe
; RUN: %t.exe

; CHECK: yansollvm: warning: mfla: skip function 'vararg_label_owner': vararg function
; CHECK: yansollvm: warning: mfla: skip function 'vararg_tab_user': vararg function
; CHECK: yansollvm: warning: mfla: skip function 'mixed_candidate': blockaddress global 'mixed_tab' mixes candidate and non-candidate labels
; CHECK: yansollvm: warning: mfla: skip function 'shared_candidate': blockaddress global 'shared_tab' is used by non-candidate code

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@mixed_tab = internal constant [2 x ptr] [
  ptr blockaddress(@mixed_candidate, %target),
  ptr blockaddress(@vararg_label_owner, %target)
]
@shared_tab = internal constant [1 x ptr] [ptr blockaddress(@shared_candidate, %target)]
@G = internal global i32 0, align 4
@llvm.compiler.used = appending global [7 x ptr] [ptr @mixed_candidate, ptr @vararg_label_owner, ptr @shared_candidate, ptr @vararg_tab_user, ptr @keep_a, ptr @keep_b, ptr @main], section "llvm.metadata"

define internal i32 @mixed_candidate(i32 %x) noinline nounwind {
entry:
  %p = load ptr, ptr @mixed_tab, align 8
  ret i32 %x
target:
  ret i32 17
}

define internal i32 @vararg_label_owner(i32 %x, ...) noinline nounwind {
entry:
  ret i32 %x
target:
  ret i32 23
}

define internal i32 @shared_candidate(i32 %x) noinline nounwind {
entry:
  %c = icmp eq i32 %x, 0
  br i1 %c, label %target, label %other
other:
  ret i32 11
target:
  ret i32 42
}

define internal i32 @vararg_tab_user(i32 %x, ...) noinline nounwind {
entry:
  %p = load ptr, ptr @shared_tab, align 8
  indirectbr ptr %p, [label %target]
target:
  ret i32 7
}

define internal i32 @keep_a(i32 %x) noinline nounwind {
entry:
  %y = add i32 %x, 1
  store i32 %y, ptr @G, align 4
  ret i32 %y
}

define internal i32 @keep_b(i32 %x) noinline nounwind {
entry:
  %y = mul i32 %x, 2
  store i32 %y, ptr @G, align 4
  ret i32 %y
}

define i32 @main() nounwind {
entry:
  %a = call i32 @keep_a(i32 1)
  %b = call i32 @keep_b(i32 3)
  %c = call i32 @mixed_candidate(i32 5)
  %oka = icmp eq i32 %a, 2
  %okb = icmp eq i32 %b, 6
  %okc = icmp eq i32 %c, 5
  %okab = and i1 %oka, %okb
  %ok = and i1 %okab, %okc
  %rc = select i1 %ok, i32 0, i32 1
  ret i32 %rc
}
