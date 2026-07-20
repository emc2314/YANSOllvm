; RUN: %opt -load-pass-plugin %plugin -passes=vm -verify-each -vm-superop-max-len=1 -S %s -o %t
; RUN: %FileCheck %s < %t

; CHECK-LABEL: define internal i1 {{@.*}}(ptr addrspace(1)
; CHECK: ptrtoint ptr addrspace(1) {{.*}} to i64
; CHECK-LABEL: define internal i1 {{@.*}}(ptr addrspace(2)
; CHECK-NOT: ptrtoint
; CHECK: icmp eq ptr addrspace(2)

target datalayout = "e-p:32:32-p1:64:64-p2:64:64-ni:2"

define i1 @integral_pointer(ptr addrspace(1) %a, ptr addrspace(1) %b) {
entry:
  %cmp = icmp eq ptr addrspace(1) %a, %b
  ret i1 %cmp
}

define i1 @non_integral_pointer(ptr addrspace(2) %a, ptr addrspace(2) %b) {
entry:
  %cmp = icmp eq ptr addrspace(2) %a, %b
  ret i1 %cmp
}

