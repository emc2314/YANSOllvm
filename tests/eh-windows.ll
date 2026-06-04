; RUN: %opt -load-pass-plugin %plugin -passes=fla -verify-each -S %s -o %t
; RUN: grep 'catchswitch' %t
; RUN: grep 'catchpad' %t
; RUN: grep 'cleanuppad' %t
; RUN: grep 'cleanupret' %t
; RUN: %lli --force-interpreter %t || test $? = 14

target triple = "x86_64-pc-windows-msvc"

define i32 @__CxxFrameHandler3(...) {
entry:
  ret i32 0
}

define i32 @may_throw(i32 %x) personality ptr @__CxxFrameHandler3 {
entry:
  %is_throw = icmp eq i32 %x, 0
  br i1 %is_throw, label %throw, label %ret

throw:
  invoke void @cleanup_probe()
          to label %ret unwind label %catch.dispatch

ret:
  ret i32 7

catch.dispatch:
  %cs = catchswitch within none [label %catch] unwind to caller

catch:
  %cp = catchpad within %cs [ptr null, i32 64, ptr null]
  catchret from %cp to label %caught

caught:
  ret i32 13
}

define void @cleanup_probe() personality ptr @__CxxFrameHandler3 {
entry:
  invoke void @never_unwinds()
          to label %ret unwind label %cleanup

ret:
  ret void

cleanup:
  %pad = cleanuppad within none []
  cleanupret from %pad unwind to caller
}

define void @never_unwinds() {
entry:
  ret void
}

define i32 @main() personality ptr @__CxxFrameHandler3 {
entry:
  %a = invoke i32 @may_throw(i32 1)
          to label %cont unwind label %catch.dispatch

cont:
  %b = call i32 @may_throw(i32 0)
  %sum = add i32 %a, %b
  ret i32 %sum

catch.dispatch:
  %cs = catchswitch within none [label %catch] unwind to caller

catch:
  %cp = catchpad within %cs [ptr null, i32 64, ptr null]
  catchret from %cp to label %caught

caught:
  ret i32 99
}
