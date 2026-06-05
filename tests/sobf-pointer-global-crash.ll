; RUN: rm -rf %t && mkdir -p %t
; RUN: %opt -load-pass-plugin %plugin -passes=sobf,verify -S %s -o %t/sobf.ll
; RUN: %clang %t/sobf.ll -o %t/sobf
; RUN: %t/sobf | grep '^hello$'

; Reduced from llvm-test-suite SingleSource/Benchmarks/BenchmarkGame/Large/fasta.c.
; A constant string can have a transitive constant global user whose value type is
; ptr. StringEncryptionPass must initialize the decrypted pointer global without
; calling ConstantAggregateZero::get(ptr).

@p = internal constant ptr @s, align 8
@s = internal constant [6 x i8] c"hello\00", align 1

define i32 @main() {
entry:
  %0 = load ptr, ptr @p, align 8
  %call = call i32 @puts(ptr %0)
  ret i32 0
}

declare i32 @puts(ptr)
