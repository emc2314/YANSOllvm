; RUN: %opt -load-pass-plugin %plugin -passes=merge,verify -S %s -o %t.out.ll
; RUN: grep 'define internal i64 @foo.bar.merge(i64' %t.out.ll
; RUN: grep 'lshr i64 %0, 32' %t.out.ll
; RUN: grep 'and i64 %0, 4294967295' %t.out.ll
; RUN: grep 'xor i64' %t.out.ll
; RUN: grep 'trunc i64' %t.out.ll
; RUN: grep 'switch i32' %t.out.ll

define internal i32 @foo(i32 %x) {
entry:
  %r = add i32 %x, 1
  ret i32 %r
}

define internal i32 @bar(i32 %x) {
entry:
  %r = mul i32 %x, 3
  ret i32 %r
}

define i32 @main(i32 %x) {
entry:
  %a = call i32 @foo(i32 %x)
  %b = call i32 @bar(i32 %a)
  ret i32 %b
}
