; RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %s -o %t.out.ll
; RUN: grep '__yansollvm_mfla_main' %t.out.ll
; RUN: grep 'mfla.tail.cur.state' %t.out.ll
; RUN: %not grep 'mfla.call.cont' %t.out.ll

; This fixture keeps an explicit self tail call in the input. MFLA should
; lower it as TailReuse: rebind args and jump to the same entry without creating
; an mfla.call.cont continuation block.

define internal i32 @tailer(i32 %x, i32 %acc) nounwind {
entry:
  %done = icmp eq i32 %x, 0
  br i1 %done, label %ret, label %recur
recur:
  %dec = add i32 %x, -1
  %sum = add i32 %acc, %x
  %r = tail call i32 @tailer(i32 %dec, i32 %sum)
  ret i32 %r
ret:
  ret i32 %acc
}

define internal i32 @dummy(i32 %x) nounwind {
entry:
  %y = add i32 %x, 7
  ret i32 %y
}
