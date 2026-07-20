; RUN: %opt -load-pass-plugin %plugin -passes=vm -verify-each -vm-superop-max-len=4 -vm-mutation-variant-permille=0 -vm-relation-app-variant-permille=0 -S %s -o %t.ll
; RUN: %FileCheck %s < %t.ll
; RUN: %clang %t.ll -o %t
; RUN: %t

; CHECK-LABEL: define i32 @diamond
; CHECK: call i32 @__yansollvm_vm_{{.*}}(i32 %a, i32 %b, i32 %c, i32 %d)
; CHECK-NEXT: ret i32

source_filename = "vm-multi-use-closure.ll"

define i32 @diamond(i32 %a, i32 %b, i32 %c, i32 %d) {
entry:
  %x = add i32 %a, %b
  %y = mul i32 %x, %c
  %z = xor i32 %x, %d
  %r = add i32 %y, %z
  ret i32 %r
}

define i32 @main() {
entry:
  %r = call i32 @diamond(i32 3, i32 4, i32 5, i32 6)
  %bad = icmp ne i32 %r, 36
  %status = zext i1 %bad to i32
  ret i32 %status
}
