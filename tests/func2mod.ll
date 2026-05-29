; RUN: rm -rf %t && mkdir -p %t
; RUN: mkdir -p %t/in %t/run
; RUN: cp %s %t/in/func2mod.ll
; RUN: cd %t/run && %opt -load-pass-plugin %plugin -passes=yanso -verify-each -func2mod -func2mod-outputs=3 -disable-output %t/in/func2mod.ll
; RUN: test -e %t/in/func2mod_main_0.bc -o -e %t/in/func2mod_main_1.bc -o -e %t/in/func2mod_main_2.bc -o -e %t/in/func2mod_main_3.bc
; RUN: test -z "$(find %t/run -maxdepth 1 -name 'func2mod_*_*.bc' -print -quit)"
; RUN: test -z "$(find %S/.. -maxdepth 1 -name 'func2mod_*_*.bc' -print -quit)"

source_filename = "func2mod.ll"

define internal i32 @helper(i32 %x) {
entry:
  %y = add i32 %x, 1
  ret i32 %y
}

define i32 @main() {
entry:
  %v = call i32 @helper(i32 41)
  ret i32 %v
}
