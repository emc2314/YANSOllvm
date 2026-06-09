; RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %s -o %t.out.ll 2>&1 | %FileCheck %s
; RUN: grep '@__yansollvm_mfla_frame = internal global \[48 x i8\]' %t.out.ll

; CHECK: yansollvm: warning: mfla: skip function 'rec': recursive SCC not supported

; A and B are both transformed candidates. A calls skipped recursive native R,
; and R may call transformed B through its ABI wrapper before returning to A.
; A and B must therefore interfere and cannot share static frame storage.

define internal i32 @b(i32 %x) nounwind {
entry:
  %b1 = add i32 %x, 11
  %b2 = mul i32 %b1, 3
  ret i32 %b2
}

define internal i32 @rec(i32 %x) nounwind {
entry:
  %done = icmp eq i32 %x, 0
  br i1 %done, label %base, label %recur
base:
  %bv = call i32 @b(i32 %x)
  ret i32 %bv
recur:
  %dec = add i32 %x, -1
  %rv = call i32 @rec(i32 %dec)
  ret i32 %rv
}

define internal i32 @a(i32 %x) nounwind {
entry:
  %pre = add i32 %x, 5
  %rv = call i32 @rec(i32 %x)
  %sum = add i32 %rv, %pre
  ret i32 %sum
}
