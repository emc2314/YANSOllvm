; RUN: %opt -load-pass-plugin %plugin -passes=merge,verify -merge-max-group-size=4 -S %s -o %t.out.ll
; RUN: %FileCheck %s --input-file=%t.out.ll

; Regression test for MergePass invoke rewriting.
;
; When a (non-candidate) caller `invoke`s a merged target with a non-void
; return, the rewrite inserts a conversion block (ConvertBB) on the normal
; edge: new-invoke -> ConvertBB -> NormalDest.  Any PHI in NormalDest must
; then take its incoming value from ConvertBB, not from the original invoke
; block, or the module fails verification with "PHI node entries do not match
; predecessors" / "instruction does not dominate all uses".

declare i32 @__gxx_personality_v0(...)

; varargs -> caller is not itself a merge candidate, so its invoke of the
; merged @leaf_a is rewritten in place.  The normal-dest block has a
; multi-incoming PHI that cannot be folded away.
define i32 @caller(i32 %x, ...) personality ptr @__gxx_personality_v0 {
entry:
  %sel = icmp sgt i32 %x, 0
  br i1 %sel, label %pre, label %direct
pre:
  br label %cont
direct:
  %r = invoke i32 @leaf_a(i32 %x) to label %cont unwind label %lp
cont:
  %p = phi i32 [ 0, %pre ], [ %r, %direct ]
  ret i32 %p
lp:
  %l = landingpad { ptr, i32 } cleanup
  ret i32 -1
}

define i32 @leaf_a(i32 %x) noinline {
  %r = add i32 %x, 10
  ret i32 %r
}
define i32 @leaf_b(i32 %x) noinline {
  %r = mul i32 %x, 3
  ret i32 %r
}
define i32 @leaf_c(i32 %x) noinline {
  %r = sub i32 %x, 5
  ret i32 %r
}

; The conversion block is interposed on the normal edge and the PHI now names
; it as the predecessor for the converted value.
; CHECK: invoke i64 @{{.*}}merge
; CHECK-NEXT: to label %[[CONV:[a-zA-Z0-9._]+]] unwind
; CHECK: [[CONV]]:
; CHECK: %[[CVT:[0-9]+]] = trunc i64 {{.*}} to i32
; CHECK: phi i32 [ 0, %pre ], [ %[[CVT]], %[[CONV]] ]
