; RUN: rm -rf %t && mkdir -p %t
; RUN: %opt -load-pass-plugin %plugin -passes=split,fla,sub -verify-each -S %s -o %t/fla.ll
; RUN: grep 'switch i64' %t/fla.ll
; RUN: grep 'hashState.ptr' %t/fla.ll
; RUN: grep 'state.ptr' %t/fla.ll
; RUN: grep '421922839' %t/fla.ll
; RUN: grep '303814201677840' %t/fla.ll
; RUN: ! grep 'select i64' %t/fla.ll
; RUN: %opt -load-pass-plugin %plugin -passes=vm -verify-each -S %s -o %t/vm.ll
; RUN: grep '__yansollvm_vm_add' %t/vm.ll
; RUN: %opt -load-pass-plugin %plugin -passes=connect -verify-each -S %s -o %t/connect.ll
; RUN: grep 'llvm.trap' %t/connect.ll

source_filename = "shape.ll"

define i32 @main(i32 %x) {
entry:
  %a = add i32 %x, 7
  %cond = icmp sgt i32 %a, 10
  br i1 %cond, label %then, label %else

then:
  %b = add i32 %a, 3
  %c = xor i32 %b, 11
  %d = mul i32 %c, 5
  %e = sub i32 %d, %x
  br label %exit

else:
  %f = add i32 %a, 13
  %g = xor i32 %f, 19
  %h = mul i32 %g, 7
  %i = sub i32 %h, %x
  br label %exit

exit:
  ret i32 0
}
