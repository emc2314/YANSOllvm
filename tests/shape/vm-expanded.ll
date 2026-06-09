; RUN: rm -rf %t && mkdir -p %t
; RUN: %opt -load-pass-plugin %plugin -passes=yanso -verify-each -vm -S %s -o %t/vm-expanded.ll
; RUN: %opt -load-pass-plugin %plugin -passes=vm,vm -verify-each -S %s -o %t/vm-twice.ll
; RUN: grep '__yansollvm_vm_Add_i32' %t/vm-twice.ll
; RUN: %opt -load-pass-plugin %plugin -passes=vm,merge -verify-each -S %s -o %t/vm-merge.ll
; RUN: grep 'merge' %t/vm-merge.ll
; RUN: %opt -load-pass-plugin %plugin -passes=yanso -verify-each -vm -icall -S %s -o %t/vm-icall.ll
; RUN: grep '__yansollvm_vm_Ctlz_i32_0' %t/vm-icall.ll
; RUN: grep '__yansollvm_vm_Add_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_Sub_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_Mul_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_UDiv_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_SDiv_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_URem_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_SRem_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_Shl_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_LShr_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_AShr_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_And_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_Or_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_Xor_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_ICmpUGT_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_FShl_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_FShr_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_BSwap_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_CtPop_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_Ctlz_i32_0' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_Cttz_i32_0' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_Abs_i32_0' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_SMin_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_UMax_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_Trunc_i32_i8' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_ZExt_i8_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_SExt_i8_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_Select_i32' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_Add_i128' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_ICmpEQ_i128' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_Select_i128' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_ICmpNE_p0' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_Select_p0' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_PtrToInt_p0_i64' %t/vm-expanded.ll
; RUN: grep '__yansollvm_vm_IntToPtr_i64_p0' %t/vm-expanded.ll
; RUN: ! grep '__yansollvm_vm_Add[^_]' %t/vm-expanded.ll
; RUN: ! grep 'call i64 @__yansollvm_vm_Add' %t/vm-expanded.ll
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
