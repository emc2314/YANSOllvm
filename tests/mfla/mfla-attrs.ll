; RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %s -o %t.out.ll 2>&1 | %FileCheck %s
; RUN: grep 'define dso_local i32 @pureish' %t.out.ll | grep -qv 'readnone'
; RUN: grep 'define dso_local i32 @pureish' %t.out.ll | grep -qv 'willreturn'
; RUN: grep 'define dso_local i32 @pureish' %t.out.ll | grep -qv 'mustprogress'
; RUN: %not grep 'define internal i32 @dead_local' %t.out.ll
; RUN: grep 'define internal void @takes_byval' %t.out.ll
; RUN: grep -qv 'call.*@takes_byval' %t.out.ll

; CHECK: yansollvm: warning: mfla: skip function 'takes_byval': unsupported ABI parameter attribute

define dso_local i32 @pureish(i32 %x) #0 {
entry:
  %a = call i32 @dead_local(i32 %x)
  %y = add i32 %a, 1
  ret i32 %y
}

define internal i32 @plain(i32 %x) {
entry:
  %y = add i32 %x, 2
  ret i32 %y
}

define internal i32 @dead_local(i32 %x) nounwind {
entry:
  %y = mul i32 %x, 3
  ret i32 %y
}

%S = type { i32, i32 }

define internal void @takes_byval(ptr byval(%S) %p) {
entry:
  ret void
}

define internal void @calls_byval(ptr %p) {
entry:
  call void @takes_byval(ptr byval(%S) %p)
  ret void
}

attributes #0 = { mustprogress nofree norecurse nosync nounwind readnone willreturn }
