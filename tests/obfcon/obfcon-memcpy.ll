; RUN: %opt -load-pass-plugin %plugin -passes=obfcon -verify-each -S %s -o %t
; RUN: %FileCheck %s < %t
; RUN: %lli %t

; CHECK-NOT: @table =
; CHECK: @table.obf.a = private unnamed_addr constant
; CHECK: @table.obf.b = private unnamed_addr constant

@table = internal global [8 x i16] [i16 4386, i16 13124, i16 21862, i16 30600,
                                     i16 -26198, i16 -17460, i16 -8722, i16 -291]

declare void @llvm.memcpy.p0.p0.i64(ptr noalias writeonly, ptr noalias readonly,
                                    i64, i1 immarg)

define void @copy_slice(ptr %dest, i64 %offset, i64 %length) {
entry:
  %base = getelementptr inbounds [8 x i16], ptr @table, i64 0, i64 0
  %source = getelementptr inbounds i8, ptr %base, i64 %offset
  call void @llvm.memcpy.p0.p0.i64(ptr %dest, ptr %source, i64 %length, i1 false)
  ret void
}

define i32 @main() {
entry:
  %buffer = alloca [5 x i8], align 1
  %dest = getelementptr inbounds [5 x i8], ptr %buffer, i64 0, i64 0
  call void @copy_slice(ptr %dest, i64 3, i64 5)
  %p0 = getelementptr inbounds i8, ptr %dest, i64 0
  %p1 = getelementptr inbounds i8, ptr %dest, i64 1
  %p2 = getelementptr inbounds i8, ptr %dest, i64 2
  %p3 = getelementptr inbounds i8, ptr %dest, i64 3
  %p4 = getelementptr inbounds i8, ptr %dest, i64 4
  %v0 = load i8, ptr %p0
  %v1 = load i8, ptr %p1
  %v2 = load i8, ptr %p2
  %v3 = load i8, ptr %p3
  %v4 = load i8, ptr %p4
  %c0 = icmp eq i8 %v0, 51
  %c1 = icmp eq i8 %v1, 102
  %c2 = icmp eq i8 %v2, 85
  %c3 = icmp eq i8 %v3, -120
  %c4 = icmp eq i8 %v4, 119
  %a0 = and i1 %c0, %c1
  %a1 = and i1 %c2, %c3
  %a2 = and i1 %a0, %a1
  %ok = and i1 %a2, %c4
  %status = select i1 %ok, i32 0, i32 1
  ret i32 %status
}
