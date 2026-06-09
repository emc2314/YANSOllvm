; RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %s -o %t.out.ll
; RUN: grep '@__yansollvm_mfla_frame = internal global \[96 x i8\]' %t.out.ll
; RUN: grep '__yansollvm_mfla_main' %t.out.ll

; A may call B or C, but B and C cannot be active at the same time.
; Their static frame regions should therefore share the same physical storage.

define internal i64 @b(i64 %x) nounwind {
entry:
  %b1 = add i64 %x, 11
  %b2 = mul i64 %b1, 3
  %b3 = xor i64 %b2, 7
  ret i64 %b3
}

define internal i64 @c(i64 %x) nounwind {
entry:
  %c1 = add i64 %x, 13
  %c2 = mul i64 %c1, 5
  %c3 = xor i64 %c2, 9
  ret i64 %c3
}

define internal i64 @a(i1 %cond, i64 %x) nounwind {
entry:
  br i1 %cond, label %bt, label %ct
bt:
  %bv = call i64 @b(i64 %x)
  br label %merge
ct:
  %cv = call i64 @c(i64 %x)
  br label %merge
merge:
  %phi = phi i64 [ %bv, %bt ], [ %cv, %ct ]
  ret i64 %phi
}
