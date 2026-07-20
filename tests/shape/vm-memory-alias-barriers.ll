; RUN: rm -rf %t && mkdir -p %t
; RUN: %opt -load-pass-plugin %plugin -passes=vm -verify-each -vm-superop-max-len=4 -S %s -o %t/vm-memory-alias-barriers.ll
; RUN: %FileCheck %s < %t/vm-memory-alias-barriers.ll
; RUN: %clang %t/vm-memory-alias-barriers.ll -o %t/vm-memory-alias-barriers
; RUN: %t/vm-memory-alias-barriers

; CHECK-LABEL: define i32 @safe_fusable
; CHECK: call i32 @__yansollvm_vm_{{.*}}(i64 %i)
; CHECK-LABEL: define i32 @store_alias_barrier
; CHECK: call i32 @__yansollvm_vm_{{.*load.*}}
; CHECK-NEXT: call void @__yansollvm_vm_{{.*store.*}}(i32 99
; CHECK-NEXT: call i32 @__yansollvm_vm_{{.*xor.*}}
; CHECK-LABEL: define i32 @call_alias_barrier
; CHECK: call i32 @__yansollvm_vm_{{.*load.*}}
; CHECK-NEXT: call void @clobber
; CHECK-NEXT: call i32 @__yansollvm_vm_{{.*xor.*}}
; CHECK-LABEL: define i32 @repeated_pattern_alias
; CHECK: call i32 @__yansollvm_vm_
; CHECK: call void @__yansollvm_vm_{{.*store.*}}(i32 123
; CHECK: call {{.*}}@__yansollvm_vm_

source_filename = "vm-memory-alias-barriers.ll"

@G = global [4 x i32] [i32 10, i32 20, i32 30, i32 40]

; The call is intentionally opaque to the VM DAG collector (non-intrinsic call)
; but has a real side effect through an alias pointer.
define void @clobber(ptr %p) noinline optnone {
entry:
  store i32 77, ptr %p, align 4
  ret void
}

; Positive case: no intervening memory op, so GEP+load+add may be fused.
define i32 @safe_fusable(i64 %i) {
entry:
  %p = getelementptr inbounds [4 x i32], ptr @G, i64 0, i64 %i
  %x = load i32, ptr %p, align 4
  %y = add i32 %x, 5
  ret i32 %y
}

; Negative case: %p and %alias are the same pointer at runtime.  The store must
; stay between the load and xor.  Fusing load+xor into a root call after the store
; would make the load observe 99 instead of the original value.
define i32 @store_alias_barrier(ptr %base, ptr %alias) {
entry:
  %p = getelementptr inbounds i32, ptr %base, i64 0
  %x = load i32, ptr %p, align 4
  store i32 99, ptr %alias, align 4
  %y = xor i32 %x, 7
  ret i32 %y
}

; Negative case: same issue, but the intervening write is hidden inside a call.
define i32 @call_alias_barrier(ptr %base, ptr %alias) {
entry:
  %p = getelementptr inbounds i32, ptr %base, i64 0
  %x = load i32, ptr %p, align 4
  call void @clobber(ptr %alias)
  %y = xor i32 %x, 7
  ret i32 %y
}

; Negative case: repeated load/calc pattern.  The first load may not be sunk
; across the aliasing store; the second load must observe the updated value.
define i32 @repeated_pattern_alias(ptr %base, ptr %alias) {
entry:
  %p0 = getelementptr inbounds i32, ptr %base, i64 0
  %a = load i32, ptr %p0, align 4
  %a2 = add i32 %a, 1
  store i32 123, ptr %alias, align 4
  %p1 = getelementptr inbounds i32, ptr %base, i64 0
  %b = load i32, ptr %p1, align 4
  %b2 = add i32 %b, 2
  %r = sub i32 %b2, %a2
  ret i32 %r
}

define i32 @main() {
entry:
  %slot = alloca i32, align 4
  store i32 10, ptr %slot, align 4
  %s = call i32 @safe_fusable(i64 2)
  %sok = icmp eq i32 %s, 35

  store i32 10, ptr %slot, align 4
  %a = call i32 @store_alias_barrier(ptr %slot, ptr %slot)
  %aok = icmp eq i32 %a, 13
  %after_store = load i32, ptr %slot, align 4
  %astoreok = icmp eq i32 %after_store, 99

  store i32 10, ptr %slot, align 4
  %b = call i32 @call_alias_barrier(ptr %slot, ptr %slot)
  %bok = icmp eq i32 %b, 13
  %after_call = load i32, ptr %slot, align 4
  %bstoreok = icmp eq i32 %after_call, 77

  store i32 10, ptr %slot, align 4
  %c = call i32 @repeated_pattern_alias(ptr %slot, ptr %slot)
  %cok = icmp eq i32 %c, 114

  %ok0 = and i1 %sok, %aok
  %ok1 = and i1 %ok0, %astoreok
  %ok2 = and i1 %ok1, %bok
  %ok3 = and i1 %ok2, %bstoreok
  %ok4 = and i1 %ok3, %cok
  br i1 %ok4, label %good, label %bad

good:
  ret i32 0

bad:
  ret i32 1
}
