; RUN: %opt -load-pass-plugin %plugin -passes=mfla,verify -S %s -o %t.out.ll 2>&1 | %FileCheck %s
; RUN: grep -qv '__yansollvm_mfla_main' %t.out.ll
; RUN: grep -qv '__yansollvm_mfla_frame' %t.out.ll
; RUN: %FileCheck %s --check-prefix=IR < %t.out.ll

; CHECK: yansollvm: warning: mfla: skip function 'caller': current lowering does not support throwing internal calls
; CHECK: yansollvm: warning: mfla: skip module '{{.*}}': fewer than two eligible functions
; IR-NOT: attributes {{.*}} nounwind

define internal void @callee() {
entry:
  ret void
}

define internal void @caller() {
entry:
  call void @callee()
  unreachable
}
