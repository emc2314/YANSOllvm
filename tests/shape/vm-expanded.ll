; RUN: rm -rf %t && mkdir -p %t
; RUN: %opt -load-pass-plugin %plugin -passes=yanso -verify-each -vm -S %s -o %t/vm-expanded.ll
; RUN: %opt -load-pass-plugin %plugin -passes=vm,vm -verify-each -S %s -o %t/vm-twice.ll
; RUN: grep '__yansollvm_vm_add_i32' %t/vm-twice.ll
; RUN: grep '_e[0-9]' %t/vm-expanded.ll
; RUN: %opt -load-pass-plugin %plugin -passes=vm -verify-each -vm-max-variants-per-op=1 -S %s -o %t/vm-cap1.ll
; RUN: test $(grep -c '^define internal i32 @__yansollvm_vm_add_i32' %t/vm-cap1.ll) -eq 1
; RUN: %opt -load-pass-plugin %plugin -passes=vm -verify-each -vm-mutation-variant-permille=1000 -S %s -o %t/vm-cf.ll
; RUN: grep 'vm.cf.loop' %t/vm-cf.ll
; RUN: grep '_mloop' %t/vm-cf.ll
; RUN: grep '_mmux' %t/vm-cf.ll
; RUN: %opt -load-pass-plugin %plugin -passes=vm -verify-each -vm-mutation-variant-permille=1000 -S %s -o %t/vm-mux.ll
; RUN: grep 'vm.mux.pred' %t/vm-mux.ll
; RUN: grep '_mmux' %t/vm-mux.ll
; RUN: grep 'vm.pred.mux\|vm.select.mux' %t/vm-mux.ll
; RUN: %opt -load-pass-plugin %plugin -passes=vm -verify-each -vm-relation-app-variant-permille=1000 -S %s -o %t/vm-id.ll
; RUN: grep 'vm.rel' %t/vm-id.ll
; RUN: grep 'vm.id.app.diff\|vm.id.app.mul\|vm.id.app.aff\|vm.id.app.fork' %t/vm-id.ll
; RUN: grep '_rpopcarry\|_rmaskpart\|_raffrt' %t/vm-id.ll
; RUN: grep 'vm.rel.maskpart\|vm.rel.aff\|vm.rel.popcarry' %t/vm-id.ll
; RUN: grep 'vm.id.app.fork.guard\|vm.id.app.fork.bad' %t/vm-id.ll
; RUN: grep 'vm.div\|vm.divrem' %t/vm-expanded.ll
; RUN: grep 'vm.intr.out' %t/vm-expanded.ll
; RUN: grep 'vm.cast.in\|vm.cast.out' %t/vm-expanded.ll
; RUN: %opt -load-pass-plugin %plugin -passes=vm,merge -verify-each -S %s -o %t/vm-merge.ll
; RUN: grep 'merge' %t/vm-merge.ll
; RUN: %opt -load-pass-plugin %plugin -passes=yanso -verify-each -vm -icall -S %s -o %t/vm-icall.ll
; RUN: grep '__yansollvm_vm_ctlz_i32_0' %t/vm-icall.ll
; RUN: grep '__yansollvm_vm_add_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_sub_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_mul_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_udiv_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_sdiv_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_urem_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_srem_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_shl_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_lshr_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_ashr_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_and_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_or_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_xor_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_icmp_ugt_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_icmp_ugt_i32_e[0-9]' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_fshl_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_fshr_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_bswap_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_ctpop_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_ctlz_i32_0' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_cttz_i32_0' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_abs_i32_0' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_smin_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_umax_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_trunc_i32_i8' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_zext_i8_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_sext_i8_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_select_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_select_i32_e[0-9]' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_add_i128' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_icmp_eq_i128' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_select_i128' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_icmp_ne_ptr' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_select_ptr' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_ptrtoint_ptr_i64' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_inttoptr_i64_ptr' %t/vm-expanded.ll
; RUN: ! grep '__yansollvm_vm_add[^_]' %t/vm-expanded.ll
; RUN: ! grep 'call i64 @__yansollvm_vm_add' %t/vm-expanded.ll
; RUN: %clang %t/vm-expanded.ll -o %t/vm-expanded
; RUN: %t/vm-expanded

source_filename = "vm-expanded.ll"

declare i32 @llvm.fshl.i32(i32, i32, i32)
declare i32 @llvm.fshr.i32(i32, i32, i32)
declare i32 @llvm.bswap.i32(i32)
declare i32 @llvm.ctpop.i32(i32)
declare i32 @llvm.ctlz.i32(i32, i1 immarg)
declare i32 @llvm.cttz.i32(i32, i1 immarg)
declare i32 @llvm.abs.i32(i32, i1 immarg)
declare i32 @llvm.smin.i32(i32, i32)
declare i32 @llvm.umax.i32(i32, i32)

@G0 = global i32 7
@G1 = global i32 11

define i32 @ops(i32 %x, i32 %y, i32 %z) {
entry:
  %add = add i32 %x, %y
  %sub = sub i32 %add, %z
  %mul = mul i32 %sub, 3
  %udiv = udiv i32 %mul, 5
  %sdiv = sdiv i32 %sub, 7
  %urem = urem i32 %mul, 11
  %srem = srem i32 %sub, 13
  %shl = shl i32 %x, 3
  %lshr = lshr i32 %y, 2
  %ashr = ashr i32 %z, 1
  %and = and i32 %shl, %lshr
  %or = or i32 %and, %ashr
  %xor = xor i32 %or, %srem
  %fshl = call i32 @llvm.fshl.i32(i32 %xor, i32 %x, i32 %z)
  %fshr = call i32 @llvm.fshr.i32(i32 %fshl, i32 %y, i32 %z)
  %bswap = call i32 @llvm.bswap.i32(i32 %fshr)
  %ctpop = call i32 @llvm.ctpop.i32(i32 %bswap)
  %ctlz = call i32 @llvm.ctlz.i32(i32 %bswap, i1 false)
  %cttz = call i32 @llvm.cttz.i32(i32 %bswap, i1 false)
  %abs = call i32 @llvm.abs.i32(i32 %sdiv, i1 false)
  %smin = call i32 @llvm.smin.i32(i32 %abs, i32 %ctpop)
  %umax = call i32 @llvm.umax.i32(i32 %ctlz, i32 %cttz)
  %r0 = add i32 %udiv, %urem
  %r1 = add i32 %r0, %xor
  %r2 = add i32 %r1, %smin
  %r3 = add i32 %r2, %umax
  %tr = trunc i32 %r3 to i8
  %ze = zext i8 %tr to i32
  %se = sext i8 %tr to i32
  %cond = icmp ugt i32 %r3, 2147483648
  %sel = select i1 %cond, i32 %se, i32 %ze
  %r4 = add i32 %r3, %sel
  ret i32 %r4
}

define i128 @wide(i128 %x, i128 %y) {
entry:
  %sum = add i128 %x, %y
  %ok = icmp eq i128 %sum, %x
  %sel = select i1 %ok, i128 %sum, i128 %y
  ret i128 %sel
}

define i32 @ptr_ops(ptr %p, ptr %q) {
entry:
  %neq = icmp ne ptr %p, %q
  %sel = select i1 %neq, ptr %p, ptr %q
  %asint = ptrtoint ptr %sel to i64
  %back = inttoptr i64 %asint to ptr
  %ok = icmp eq ptr %back, %sel
  %ret = select i1 %ok, i32 17, i32 19
  ret i32 %ret
}

define i32 @main() {
entry:
  %r = call i32 @ops(i32 305419896, i32 -1698898192, i32 5)
  %w = call i128 @wide(i128 340282366920938463463374607431768211455, i128 1)
  %p = call i32 @ptr_ops(ptr @G0, ptr @G1)
  %wok = icmp eq i128 %w, 1
  %pok = icmp eq i32 %p, 17
  %ok = icmp eq i32 %r, 12276492
  %all0 = and i1 %ok, %wok
  %allok = and i1 %all0, %pok
  br i1 %allok, label %good, label %bad

good:
  ret i32 0

bad:
  ret i32 1
}
