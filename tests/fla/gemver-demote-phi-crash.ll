; RUN: %opt -load-pass-plugin %plugin -passes=fla -verify-each -S %s -o %t
; ModuleID = '/root/yansollvm/work/llvm-test-suite/SingleSource/Benchmarks/Polybench/linear-algebra/blas/gemver/gemver.c'
source_filename = "/root/yansollvm/work/llvm-test-suite/SingleSource/Benchmarks/Polybench/linear-algebra/blas/gemver/gemver.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@polybench_papi_counters_threadid = dso_local local_unnamed_addr global i32 0, align 4
@polybench_program_total_flops = dso_local local_unnamed_addr global double 0.000000e+00, align 8
@polybench_t_start = dso_local local_unnamed_addr global double 0.000000e+00, align 8
@polybench_t_end = dso_local local_unnamed_addr global double 0.000000e+00, align 8
@.str = private unnamed_addr constant [7 x i8] c"%0.6f\0A\00", align 1
@polybench_c_start = dso_local local_unnamed_addr global i64 0, align 8
@polybench_c_end = dso_local local_unnamed_addr global i64 0, align 8
@stderr = external local_unnamed_addr global ptr, align 8
@.str.1 = private unnamed_addr constant [51 x i8] c"[PolyBench] posix_memalign: cannot allocate memory\00", align 1
@.str.2 = private unnamed_addr constant [23 x i8] c"==BEGIN DUMP_ARRAYS==\0A\00", align 1
@.str.3 = private unnamed_addr constant [15 x i8] c"begin dump: %s\00", align 1
@.str.4 = private unnamed_addr constant [2 x i8] c"w\00", align 1
@.str.6 = private unnamed_addr constant [8 x i8] c"%0.2lf \00", align 1
@.str.7 = private unnamed_addr constant [17 x i8] c"\0Aend   dump: %s\0A\00", align 1
@.str.8 = private unnamed_addr constant [23 x i8] c"==END   DUMP_ARRAYS==\0A\00", align 1

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable
define dso_local void @polybench_flush_cache() local_unnamed_addr #0 {
entry:
  ret void
}

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(i64 immarg, ptr captures(none)) #1

; Function Attrs: mustprogress nounwind willreturn allockind("free") memory(argmem: readwrite, inaccessiblemem: readwrite)
declare void @free(ptr allocptr noundef captures(none)) local_unnamed_addr #2

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(i64 immarg, ptr captures(none)) #1

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable
define dso_local void @polybench_prepare_instruments() local_unnamed_addr #0 {
entry:
  ret void
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(write, argmem: none, inaccessiblemem: none) uwtable
define dso_local void @polybench_timer_start() local_unnamed_addr #3 {
entry:
  store double 0.000000e+00, ptr @polybench_t_start, align 8, !tbaa !5
  ret void
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(write, argmem: none, inaccessiblemem: none) uwtable
define dso_local void @polybench_timer_stop() local_unnamed_addr #3 {
entry:
  store double 0.000000e+00, ptr @polybench_t_end, align 8, !tbaa !5
  ret void
}

; Function Attrs: nofree nounwind uwtable
define dso_local void @polybench_timer_print() local_unnamed_addr #4 {
entry:
  %0 = load double, ptr @polybench_t_end, align 8, !tbaa !5
  %1 = load double, ptr @polybench_t_start, align 8, !tbaa !5
  %sub = fsub double %0, %1
  %call = tail call i32 (ptr, ...) @printf(ptr noundef nonnull dereferenceable(1) @.str, double noundef %sub)
  ret void
}

; Function Attrs: nofree nounwind
declare noundef i32 @printf(ptr noundef readonly captures(none), ...) local_unnamed_addr #5

; Function Attrs: mustprogress nounwind willreturn memory(argmem: readwrite, inaccessiblemem: readwrite) uwtable
define dso_local void @polybench_free_data(ptr noundef captures(none) %ptr) local_unnamed_addr #6 {
entry:
  tail call void @free(ptr noundef %ptr) #11
  ret void
}

; Function Attrs: nofree nounwind uwtable
define dso_local nonnull ptr @polybench_alloc_data(i64 noundef %n, i32 noundef %elt_size) local_unnamed_addr #4 {
entry:
  %ret.i = alloca ptr, align 8
  %conv = sext i32 %elt_size to i64
  %mul = mul i64 %n, %conv
  call void @llvm.lifetime.start.p0(i64 8, ptr nonnull %ret.i) #11
  store ptr null, ptr %ret.i, align 8, !tbaa !9
  %call.i = call i32 @posix_memalign(ptr noundef nonnull %ret.i, i64 noundef 4096, i64 noundef %mul) #11
  %0 = load ptr, ptr %ret.i, align 8, !tbaa !9
  %tobool.i = icmp eq ptr %0, null
  %tobool2.i = icmp ne i32 %call.i, 0
  %or.cond.i = select i1 %tobool.i, i1 true, i1 %tobool2.i
  br i1 %or.cond.i, label %if.then.i, label %xmalloc.exit

if.then.i:                                        ; preds = %entry
  %1 = load ptr, ptr @stderr, align 8, !tbaa !11
  %2 = call i64 @fwrite(ptr nonnull @.str.1, i64 50, i64 1, ptr %1) #12
  call void @exit(i32 noundef 1) #13
  unreachable

xmalloc.exit:                                     ; preds = %entry
  call void @llvm.lifetime.end.p0(i64 8, ptr nonnull %ret.i) #11
  ret ptr %0
}

; Function Attrs: nounwind uwtable
define dso_local noundef i32 @main(i32 noundef %argc, ptr noundef readnone captures(none) %argv) local_unnamed_addr #7 {
entry:
  %ret.i.i97 = alloca ptr, align 8
  %ret.i.i90 = alloca ptr, align 8
  %ret.i.i83 = alloca ptr, align 8
  %ret.i.i76 = alloca ptr, align 8
  %ret.i.i69 = alloca ptr, align 8
  %ret.i.i62 = alloca ptr, align 8
  %ret.i.i55 = alloca ptr, align 8
  %ret.i.i48 = alloca ptr, align 8
  %ret.i.i = alloca ptr, align 8
  call void @llvm.lifetime.start.p0(i64 8, ptr nonnull %ret.i.i) #11
  store ptr null, ptr %ret.i.i, align 8, !tbaa !9
  %call.i.i = call i32 @posix_memalign(ptr noundef nonnull %ret.i.i, i64 noundef 4096, i64 noundef 32000000) #11
  %0 = load ptr, ptr %ret.i.i, align 8, !tbaa !9
  %tobool.i.i = icmp eq ptr %0, null
  %tobool2.i.i = icmp ne i32 %call.i.i, 0
  %or.cond.i.i = select i1 %tobool.i.i, i1 true, i1 %tobool2.i.i
  br i1 %or.cond.i.i, label %if.then.i.i, label %polybench_alloc_data.exit

if.then.i.i:                                      ; preds = %entry
  %1 = load ptr, ptr @stderr, align 8, !tbaa !11
  %2 = call i64 @fwrite(ptr nonnull @.str.1, i64 50, i64 1, ptr %1) #12
  call void @exit(i32 noundef 1) #13
  unreachable

polybench_alloc_data.exit:                        ; preds = %entry
  call void @llvm.lifetime.end.p0(i64 8, ptr nonnull %ret.i.i) #11
  call void @llvm.lifetime.start.p0(i64 8, ptr nonnull %ret.i.i48) #11
  store ptr null, ptr %ret.i.i48, align 8, !tbaa !9
  %call.i.i49 = call i32 @posix_memalign(ptr noundef nonnull %ret.i.i48, i64 noundef 4096, i64 noundef 16000) #11
  %3 = load ptr, ptr %ret.i.i48, align 8, !tbaa !9
  %tobool.i.i50 = icmp eq ptr %3, null
  %tobool2.i.i51 = icmp ne i32 %call.i.i49, 0
  %or.cond.i.i52 = select i1 %tobool.i.i50, i1 true, i1 %tobool2.i.i51
  br i1 %or.cond.i.i52, label %if.then.i.i53, label %polybench_alloc_data.exit54

if.then.i.i53:                                    ; preds = %polybench_alloc_data.exit
  %4 = load ptr, ptr @stderr, align 8, !tbaa !11
  %5 = call i64 @fwrite(ptr nonnull @.str.1, i64 50, i64 1, ptr %4) #12
  call void @exit(i32 noundef 1) #13
  unreachable

polybench_alloc_data.exit54:                      ; preds = %polybench_alloc_data.exit
  call void @llvm.lifetime.end.p0(i64 8, ptr nonnull %ret.i.i48) #11
  call void @llvm.lifetime.start.p0(i64 8, ptr nonnull %ret.i.i55) #11
  store ptr null, ptr %ret.i.i55, align 8, !tbaa !9
  %call.i.i56 = call i32 @posix_memalign(ptr noundef nonnull %ret.i.i55, i64 noundef 4096, i64 noundef 16000) #11
  %6 = load ptr, ptr %ret.i.i55, align 8, !tbaa !9
  %tobool.i.i57 = icmp eq ptr %6, null
  %tobool2.i.i58 = icmp ne i32 %call.i.i56, 0
  %or.cond.i.i59 = select i1 %tobool.i.i57, i1 true, i1 %tobool2.i.i58
  br i1 %or.cond.i.i59, label %if.then.i.i60, label %polybench_alloc_data.exit61

if.then.i.i60:                                    ; preds = %polybench_alloc_data.exit54
  %7 = load ptr, ptr @stderr, align 8, !tbaa !11
  %8 = call i64 @fwrite(ptr nonnull @.str.1, i64 50, i64 1, ptr %7) #12
  call void @exit(i32 noundef 1) #13
  unreachable

polybench_alloc_data.exit61:                      ; preds = %polybench_alloc_data.exit54
  call void @llvm.lifetime.end.p0(i64 8, ptr nonnull %ret.i.i55) #11
  call void @llvm.lifetime.start.p0(i64 8, ptr nonnull %ret.i.i62) #11
  store ptr null, ptr %ret.i.i62, align 8, !tbaa !9
  %call.i.i63 = call i32 @posix_memalign(ptr noundef nonnull %ret.i.i62, i64 noundef 4096, i64 noundef 16000) #11
  %9 = load ptr, ptr %ret.i.i62, align 8, !tbaa !9
  %tobool.i.i64 = icmp eq ptr %9, null
  %tobool2.i.i65 = icmp ne i32 %call.i.i63, 0
  %or.cond.i.i66 = select i1 %tobool.i.i64, i1 true, i1 %tobool2.i.i65
  br i1 %or.cond.i.i66, label %if.then.i.i67, label %polybench_alloc_data.exit68

if.then.i.i67:                                    ; preds = %polybench_alloc_data.exit61
  %10 = load ptr, ptr @stderr, align 8, !tbaa !11
  %11 = call i64 @fwrite(ptr nonnull @.str.1, i64 50, i64 1, ptr %10) #12
  call void @exit(i32 noundef 1) #13
  unreachable

polybench_alloc_data.exit68:                      ; preds = %polybench_alloc_data.exit61
  call void @llvm.lifetime.end.p0(i64 8, ptr nonnull %ret.i.i62) #11
  call void @llvm.lifetime.start.p0(i64 8, ptr nonnull %ret.i.i69) #11
  store ptr null, ptr %ret.i.i69, align 8, !tbaa !9
  %call.i.i70 = call i32 @posix_memalign(ptr noundef nonnull %ret.i.i69, i64 noundef 4096, i64 noundef 16000) #11
  %12 = load ptr, ptr %ret.i.i69, align 8, !tbaa !9
  %tobool.i.i71 = icmp eq ptr %12, null
  %tobool2.i.i72 = icmp ne i32 %call.i.i70, 0
  %or.cond.i.i73 = select i1 %tobool.i.i71, i1 true, i1 %tobool2.i.i72
  br i1 %or.cond.i.i73, label %if.then.i.i74, label %polybench_alloc_data.exit75

if.then.i.i74:                                    ; preds = %polybench_alloc_data.exit68
  %13 = load ptr, ptr @stderr, align 8, !tbaa !11
  %14 = call i64 @fwrite(ptr nonnull @.str.1, i64 50, i64 1, ptr %13) #12
  call void @exit(i32 noundef 1) #13
  unreachable

polybench_alloc_data.exit75:                      ; preds = %polybench_alloc_data.exit68
  call void @llvm.lifetime.end.p0(i64 8, ptr nonnull %ret.i.i69) #11
  call void @llvm.lifetime.start.p0(i64 8, ptr nonnull %ret.i.i76) #11
  store ptr null, ptr %ret.i.i76, align 8, !tbaa !9
  %call.i.i77 = call i32 @posix_memalign(ptr noundef nonnull %ret.i.i76, i64 noundef 4096, i64 noundef 16000) #11
  %15 = load ptr, ptr %ret.i.i76, align 8, !tbaa !9
  %tobool.i.i78 = icmp eq ptr %15, null
  %tobool2.i.i79 = icmp ne i32 %call.i.i77, 0
  %or.cond.i.i80 = select i1 %tobool.i.i78, i1 true, i1 %tobool2.i.i79
  br i1 %or.cond.i.i80, label %if.then.i.i81, label %polybench_alloc_data.exit82

if.then.i.i81:                                    ; preds = %polybench_alloc_data.exit75
  %16 = load ptr, ptr @stderr, align 8, !tbaa !11
  %17 = call i64 @fwrite(ptr nonnull @.str.1, i64 50, i64 1, ptr %16) #12
  call void @exit(i32 noundef 1) #13
  unreachable

polybench_alloc_data.exit82:                      ; preds = %polybench_alloc_data.exit75
  call void @llvm.lifetime.end.p0(i64 8, ptr nonnull %ret.i.i76) #11
  call void @llvm.lifetime.start.p0(i64 8, ptr nonnull %ret.i.i83) #11
  store ptr null, ptr %ret.i.i83, align 8, !tbaa !9
  %call.i.i84 = call i32 @posix_memalign(ptr noundef nonnull %ret.i.i83, i64 noundef 4096, i64 noundef 16000) #11
  %18 = load ptr, ptr %ret.i.i83, align 8, !tbaa !9
  %tobool.i.i85 = icmp eq ptr %18, null
  %tobool2.i.i86 = icmp ne i32 %call.i.i84, 0
  %or.cond.i.i87 = select i1 %tobool.i.i85, i1 true, i1 %tobool2.i.i86
  br i1 %or.cond.i.i87, label %if.then.i.i88, label %polybench_alloc_data.exit89

if.then.i.i88:                                    ; preds = %polybench_alloc_data.exit82
  %19 = load ptr, ptr @stderr, align 8, !tbaa !11
  %20 = call i64 @fwrite(ptr nonnull @.str.1, i64 50, i64 1, ptr %19) #12
  call void @exit(i32 noundef 1) #13
  unreachable

polybench_alloc_data.exit89:                      ; preds = %polybench_alloc_data.exit82
  call void @llvm.lifetime.end.p0(i64 8, ptr nonnull %ret.i.i83) #11
  call void @llvm.lifetime.start.p0(i64 8, ptr nonnull %ret.i.i90) #11
  store ptr null, ptr %ret.i.i90, align 8, !tbaa !9
  %call.i.i91 = call i32 @posix_memalign(ptr noundef nonnull %ret.i.i90, i64 noundef 4096, i64 noundef 16000) #11
  %21 = load ptr, ptr %ret.i.i90, align 8, !tbaa !9
  %tobool.i.i92 = icmp eq ptr %21, null
  %tobool2.i.i93 = icmp ne i32 %call.i.i91, 0
  %or.cond.i.i94 = select i1 %tobool.i.i92, i1 true, i1 %tobool2.i.i93
  br i1 %or.cond.i.i94, label %if.then.i.i95, label %polybench_alloc_data.exit96

if.then.i.i95:                                    ; preds = %polybench_alloc_data.exit89
  %22 = load ptr, ptr @stderr, align 8, !tbaa !11
  %23 = call i64 @fwrite(ptr nonnull @.str.1, i64 50, i64 1, ptr %22) #12
  call void @exit(i32 noundef 1) #13
  unreachable

polybench_alloc_data.exit96:                      ; preds = %polybench_alloc_data.exit89
  call void @llvm.lifetime.end.p0(i64 8, ptr nonnull %ret.i.i90) #11
  call void @llvm.lifetime.start.p0(i64 8, ptr nonnull %ret.i.i97) #11
  store ptr null, ptr %ret.i.i97, align 8, !tbaa !9
  %call.i.i98 = call i32 @posix_memalign(ptr noundef nonnull %ret.i.i97, i64 noundef 4096, i64 noundef 16000) #11
  %24 = load ptr, ptr %ret.i.i97, align 8, !tbaa !9
  %tobool.i.i99 = icmp eq ptr %24, null
  %tobool2.i.i100 = icmp ne i32 %call.i.i98, 0
  %or.cond.i.i101 = select i1 %tobool.i.i99, i1 true, i1 %tobool2.i.i100
  br i1 %or.cond.i.i101, label %if.then.i.i102, label %polybench_alloc_data.exit103

if.then.i.i102:                                   ; preds = %polybench_alloc_data.exit96
  %25 = load ptr, ptr @stderr, align 8, !tbaa !11
  %26 = call i64 @fwrite(ptr nonnull @.str.1, i64 50, i64 1, ptr %25) #12
  call void @exit(i32 noundef 1) #13
  unreachable

polybench_alloc_data.exit103:                     ; preds = %polybench_alloc_data.exit96
  call void @llvm.lifetime.end.p0(i64 8, ptr nonnull %ret.i.i97) #11
  br label %for.body.i

for.cond1.preheader.i.preheader:                  ; preds = %middle.block
  %scevgep = getelementptr i8, ptr %0, i64 32000000
  %27 = insertelement <4 x ptr> poison, ptr %3, i64 0
  %28 = insertelement <4 x ptr> %27, ptr %6, i64 1
  %29 = insertelement <4 x ptr> %28, ptr %9, i64 2
  %30 = insertelement <4 x ptr> %29, ptr %12, i64 3
  %31 = getelementptr i8, <4 x ptr> %30, i64 16000
  %32 = insertelement <4 x ptr> poison, ptr %0, i64 0
  %33 = shufflevector <4 x ptr> %32, <4 x ptr> poison, <4 x i32> zeroinitializer
  %34 = insertelement <4 x ptr> poison, ptr %scevgep, i64 0
  %35 = shufflevector <4 x ptr> %34, <4 x ptr> poison, <4 x i32> zeroinitializer
  %36 = icmp ult <4 x ptr> %33, %31
  %37 = icmp ult <4 x ptr> %30, %35
  %38 = and <4 x i1> %36, %37
  %39 = bitcast <4 x i1> %38 to i4
  %.not = icmp eq i4 %39, 0
  br label %for.cond1.preheader.i

for.body.i:                                       ; preds = %middle.block, %polybench_alloc_data.exit103
  %indvars.iv81.i = phi i64 [ 0, %polybench_alloc_data.exit103 ], [ %indvars.iv.next82.i, %middle.block ]
  %40 = trunc nuw nsw i64 %indvars.iv81.i to i32
  %conv2.i = uitofp nneg i32 %40 to double
  %arrayidx.i = getelementptr inbounds nuw double, ptr %3, i64 %indvars.iv81.i
  store double %conv2.i, ptr %arrayidx.i, align 8, !tbaa !5
  %indvars.iv.next82.i = add nuw nsw i64 %indvars.iv81.i, 1
  %41 = trunc nuw nsw i64 %indvars.iv.next82.i to i32
  %conv3.i = uitofp nneg i32 %41 to double
  %div.i = fdiv double %conv3.i, 2.000000e+03
  %div4.i = fmul double %div.i, 5.000000e-01
  %arrayidx6.i = getelementptr inbounds nuw double, ptr %9, i64 %indvars.iv81.i
  store double %div4.i, ptr %arrayidx6.i, align 8, !tbaa !5
  %div10.i = fmul double %div.i, 2.500000e-01
  %arrayidx12.i = getelementptr inbounds nuw double, ptr %6, i64 %indvars.iv81.i
  store double %div10.i, ptr %arrayidx12.i, align 8, !tbaa !5
  %div16.i = fdiv double %div.i, 6.000000e+00
  %arrayidx18.i = getelementptr inbounds nuw double, ptr %12, i64 %indvars.iv81.i
  store double %div16.i, ptr %arrayidx18.i, align 8, !tbaa !5
  %div22.i = fmul double %div.i, 1.250000e-01
  %arrayidx24.i = getelementptr inbounds nuw double, ptr %21, i64 %indvars.iv81.i
  store double %div22.i, ptr %arrayidx24.i, align 8, !tbaa !5
  %div28.i = fdiv double %div.i, 9.000000e+00
  %arrayidx30.i = getelementptr inbounds nuw double, ptr %24, i64 %indvars.iv81.i
  store double %div28.i, ptr %arrayidx30.i, align 8, !tbaa !5
  %arrayidx32.i = getelementptr inbounds nuw double, ptr %18, i64 %indvars.iv81.i
  store double 0.000000e+00, ptr %arrayidx32.i, align 8, !tbaa !5
  %arrayidx34.i = getelementptr inbounds nuw double, ptr %15, i64 %indvars.iv81.i
  store double 0.000000e+00, ptr %arrayidx34.i, align 8, !tbaa !5
  %broadcast.splatinsert = insertelement <2 x i64> poison, i64 %indvars.iv81.i, i64 0
  %broadcast.splat = shufflevector <2 x i64> %broadcast.splatinsert, <2 x i64> poison, <2 x i32> zeroinitializer
  br label %vector.body

vector.body:                                      ; preds = %vector.body, %for.body.i
  %index = phi i64 [ 0, %for.body.i ], [ %index.next, %vector.body ]
  %vec.ind = phi <2 x i64> [ <i64 0, i64 1>, %for.body.i ], [ %vec.ind.next, %vector.body ]
  %42 = mul nuw nsw <2 x i64> %vec.ind, %broadcast.splat
  %43 = trunc nuw nsw <2 x i64> %42 to <2 x i32>
  %44 = urem <2 x i32> %43, splat (i32 2000)
  %45 = uitofp nneg <2 x i32> %44 to <2 x double>
  %46 = fdiv <2 x double> %45, splat (double 2.000000e+03)
  %47 = getelementptr inbounds nuw [2000 x double], ptr %0, i64 %indvars.iv81.i, i64 %index
  store <2 x double> %46, ptr %47, align 8, !tbaa !5
  %index.next = add nuw i64 %index, 2
  %vec.ind.next = add <2 x i64> %vec.ind, splat (i64 2)
  %48 = icmp eq i64 %index.next, 2000
  br i1 %48, label %middle.block, label %vector.body, !llvm.loop !13

middle.block:                                     ; preds = %vector.body
  %exitcond84.not.i = icmp eq i64 %indvars.iv.next82.i, 2000
  br i1 %exitcond84.not.i, label %for.cond1.preheader.i.preheader, label %for.body.i, !llvm.loop !17

for.cond1.preheader.i:                            ; preds = %for.cond1.preheader.i.preheader, %for.inc20.i
  %indvars.iv145.i = phi i64 [ %indvars.iv.next146.i, %for.inc20.i ], [ 0, %for.cond1.preheader.i.preheader ]
  %arrayidx7.i = getelementptr inbounds nuw double, ptr %3, i64 %indvars.iv145.i
  %arrayidx11.i = getelementptr inbounds nuw double, ptr %9, i64 %indvars.iv145.i
  br i1 %.not, label %vector.body126.preheader, label %for.body3.i

vector.body126.preheader:                         ; preds = %for.cond1.preheader.i
  %49 = load double, ptr %arrayidx7.i, align 8, !tbaa !5, !alias.scope !18
  %broadcast.splatinsert131 = insertelement <2 x double> poison, double %49, i64 0
  %broadcast.splat132 = shufflevector <2 x double> %broadcast.splatinsert131, <2 x double> poison, <2 x i32> zeroinitializer
  %50 = load double, ptr %arrayidx11.i, align 8, !tbaa !5, !alias.scope !21
  %broadcast.splatinsert135 = insertelement <2 x double> poison, double %50, i64 0
  %broadcast.splat136 = shufflevector <2 x double> %broadcast.splatinsert135, <2 x double> poison, <2 x i32> zeroinitializer
  br label %vector.body126

vector.body126:                                   ; preds = %vector.body126.preheader, %vector.body126
  %index127 = phi i64 [ %index.next137, %vector.body126 ], [ 0, %vector.body126.preheader ]
  %51 = getelementptr inbounds nuw [2000 x double], ptr %0, i64 %indvars.iv145.i, i64 %index127
  %52 = getelementptr inbounds nuw i8, ptr %51, i64 16
  %wide.load = load <2 x double>, ptr %51, align 8, !tbaa !5, !alias.scope !23, !noalias !25
  %wide.load128 = load <2 x double>, ptr %52, align 8, !tbaa !5, !alias.scope !23, !noalias !25
  %53 = getelementptr inbounds nuw double, ptr %6, i64 %index127
  %54 = getelementptr inbounds nuw i8, ptr %53, i64 16
  %wide.load129 = load <2 x double>, ptr %53, align 8, !tbaa !5, !alias.scope !28
  %wide.load130 = load <2 x double>, ptr %54, align 8, !tbaa !5, !alias.scope !28
  %55 = fmul <2 x double> %broadcast.splat132, %wide.load129
  %56 = fmul <2 x double> %broadcast.splat132, %wide.load130
  %57 = fadd <2 x double> %wide.load, %55
  %58 = fadd <2 x double> %wide.load128, %56
  %59 = getelementptr inbounds nuw double, ptr %12, i64 %index127
  %60 = getelementptr inbounds nuw i8, ptr %59, i64 16
  %wide.load133 = load <2 x double>, ptr %59, align 8, !tbaa !5, !alias.scope !29
  %wide.load134 = load <2 x double>, ptr %60, align 8, !tbaa !5, !alias.scope !29
  %61 = fmul <2 x double> %broadcast.splat136, %wide.load133
  %62 = fmul <2 x double> %broadcast.splat136, %wide.load134
  %63 = fadd <2 x double> %57, %61
  %64 = fadd <2 x double> %58, %62
  store <2 x double> %63, ptr %51, align 8, !tbaa !5, !alias.scope !23, !noalias !25
  store <2 x double> %64, ptr %52, align 8, !tbaa !5, !alias.scope !23, !noalias !25
  %index.next137 = add nuw i64 %index127, 4
  %65 = icmp eq i64 %index.next137, 2000
  br i1 %65, label %for.inc20.i, label %vector.body126, !llvm.loop !30

for.body3.i:                                      ; preds = %for.cond1.preheader.i, %for.body3.i
  %indvars.iv.i104 = phi i64 [ %indvars.iv.next.i105.1, %for.body3.i ], [ 0, %for.cond1.preheader.i ]
  %arrayidx5.i = getelementptr inbounds nuw [2000 x double], ptr %0, i64 %indvars.iv145.i, i64 %indvars.iv.i104
  %66 = load double, ptr %arrayidx5.i, align 8, !tbaa !5
  %67 = load double, ptr %arrayidx7.i, align 8, !tbaa !5
  %arrayidx9.i = getelementptr inbounds nuw double, ptr %6, i64 %indvars.iv.i104
  %68 = load double, ptr %arrayidx9.i, align 8, !tbaa !5
  %mul.i = fmul double %67, %68
  %add.i = fadd double %66, %mul.i
  %69 = load double, ptr %arrayidx11.i, align 8, !tbaa !5
  %arrayidx13.i = getelementptr inbounds nuw double, ptr %12, i64 %indvars.iv.i104
  %70 = load double, ptr %arrayidx13.i, align 8, !tbaa !5
  %mul14.i = fmul double %69, %70
  %add15.i = fadd double %add.i, %mul14.i
  store double %add15.i, ptr %arrayidx5.i, align 8, !tbaa !5
  %indvars.iv.next.i105 = or disjoint i64 %indvars.iv.i104, 1
  %arrayidx5.i.1 = getelementptr inbounds nuw [2000 x double], ptr %0, i64 %indvars.iv145.i, i64 %indvars.iv.next.i105
  %71 = load double, ptr %arrayidx5.i.1, align 8, !tbaa !5
  %72 = load double, ptr %arrayidx7.i, align 8, !tbaa !5
  %arrayidx9.i.1 = getelementptr inbounds nuw double, ptr %6, i64 %indvars.iv.next.i105
  %73 = load double, ptr %arrayidx9.i.1, align 8, !tbaa !5
  %mul.i.1 = fmul double %72, %73
  %add.i.1 = fadd double %71, %mul.i.1
  %74 = load double, ptr %arrayidx11.i, align 8, !tbaa !5
  %arrayidx13.i.1 = getelementptr inbounds nuw double, ptr %12, i64 %indvars.iv.next.i105
  %75 = load double, ptr %arrayidx13.i.1, align 8, !tbaa !5
  %mul14.i.1 = fmul double %74, %75
  %add15.i.1 = fadd double %add.i.1, %mul14.i.1
  store double %add15.i.1, ptr %arrayidx5.i.1, align 8, !tbaa !5
  %indvars.iv.next.i105.1 = add nuw nsw i64 %indvars.iv.i104, 2
  %exitcond.not.i106.1 = icmp eq i64 %indvars.iv.next.i105.1, 2000
  br i1 %exitcond.not.i106.1, label %for.inc20.i, label %for.body3.i, !llvm.loop !31

for.inc20.i:                                      ; preds = %vector.body126, %for.body3.i
  %indvars.iv.next146.i = add nuw nsw i64 %indvars.iv145.i, 1
  %exitcond148.not.i = icmp eq i64 %indvars.iv.next146.i, 2000
  br i1 %exitcond148.not.i, label %for.cond26.preheader.i, label %for.cond1.preheader.i, !llvm.loop !32

for.cond26.preheader.i:                           ; preds = %for.inc20.i, %for.inc45.i
  %indvars.iv153.i = phi i64 [ %indvars.iv.next154.i, %for.inc45.i ], [ 0, %for.inc20.i ]
  %arrayidx30.i107 = getelementptr inbounds nuw double, ptr %18, i64 %indvars.iv153.i
  %arrayidx30.promoted.i = load double, ptr %arrayidx30.i107, align 8, !tbaa !5
  br label %for.body28.i

for.body28.i:                                     ; preds = %for.body28.i, %for.cond26.preheader.i
  %indvars.iv149.i = phi i64 [ 0, %for.cond26.preheader.i ], [ %indvars.iv.next150.i.1, %for.body28.i ]
  %add39135136.i = phi double [ %arrayidx30.promoted.i, %for.cond26.preheader.i ], [ %add39.i.1, %for.body28.i ]
  %arrayidx34.i108 = getelementptr inbounds nuw [2000 x double], ptr %0, i64 %indvars.iv149.i, i64 %indvars.iv153.i
  %76 = load double, ptr %arrayidx34.i108, align 8, !tbaa !5
  %mul35.i = fmul double %76, 1.200000e+00
  %arrayidx37.i = getelementptr inbounds nuw double, ptr %21, i64 %indvars.iv149.i
  %77 = load double, ptr %arrayidx37.i, align 8, !tbaa !5
  %mul38.i = fmul double %mul35.i, %77
  %add39.i = fadd double %add39135136.i, %mul38.i
  store double %add39.i, ptr %arrayidx30.i107, align 8, !tbaa !5
  %indvars.iv.next150.i = or disjoint i64 %indvars.iv149.i, 1
  %arrayidx34.i108.1 = getelementptr inbounds nuw [2000 x double], ptr %0, i64 %indvars.iv.next150.i, i64 %indvars.iv153.i
  %78 = load double, ptr %arrayidx34.i108.1, align 8, !tbaa !5
  %mul35.i.1 = fmul double %78, 1.200000e+00
  %arrayidx37.i.1 = getelementptr inbounds nuw double, ptr %21, i64 %indvars.iv.next150.i
  %79 = load double, ptr %arrayidx37.i.1, align 8, !tbaa !5
  %mul38.i.1 = fmul double %mul35.i.1, %79
  %add39.i.1 = fadd double %add39.i, %mul38.i.1
  store double %add39.i.1, ptr %arrayidx30.i107, align 8, !tbaa !5
  %indvars.iv.next150.i.1 = add nuw nsw i64 %indvars.iv149.i, 2
  %exitcond152.not.i.1 = icmp eq i64 %indvars.iv.next150.i.1, 2000
  br i1 %exitcond152.not.i.1, label %for.inc45.i, label %for.body28.i, !llvm.loop !33

for.inc45.i:                                      ; preds = %for.body28.i
  %indvars.iv.next154.i = add nuw nsw i64 %indvars.iv153.i, 1
  %exitcond156.not.i = icmp eq i64 %indvars.iv.next154.i, 2000
  br i1 %exitcond156.not.i, label %vector.memcheck140, label %for.cond26.preheader.i, !llvm.loop !34

vector.memcheck140:                               ; preds = %for.inc45.i
  %scevgep141 = getelementptr i8, ptr %18, i64 16000
  %scevgep142 = getelementptr i8, ptr %24, i64 16000
  %bound0143 = icmp ult ptr %18, %scevgep142
  %bound1144 = icmp ult ptr %24, %scevgep141
  %found.conflict145 = and i1 %bound0143, %bound1144
  br i1 %found.conflict145, label %for.body50.i, label %vector.body148

vector.body148:                                   ; preds = %vector.memcheck140, %vector.body148
  %index149 = phi i64 [ %index.next154.1, %vector.body148 ], [ 0, %vector.memcheck140 ]
  %80 = getelementptr inbounds nuw double, ptr %18, i64 %index149
  %81 = getelementptr inbounds nuw i8, ptr %80, i64 16
  %wide.load150 = load <2 x double>, ptr %80, align 8, !tbaa !5, !alias.scope !35, !noalias !38
  %wide.load151 = load <2 x double>, ptr %81, align 8, !tbaa !5, !alias.scope !35, !noalias !38
  %82 = getelementptr inbounds nuw double, ptr %24, i64 %index149
  %83 = getelementptr inbounds nuw i8, ptr %82, i64 16
  %wide.load152 = load <2 x double>, ptr %82, align 8, !tbaa !5, !alias.scope !38
  %wide.load153 = load <2 x double>, ptr %83, align 8, !tbaa !5, !alias.scope !38
  %84 = fadd <2 x double> %wide.load150, %wide.load152
  %85 = fadd <2 x double> %wide.load151, %wide.load153
  store <2 x double> %84, ptr %80, align 8, !tbaa !5, !alias.scope !35, !noalias !38
  store <2 x double> %85, ptr %81, align 8, !tbaa !5, !alias.scope !35, !noalias !38
  %index.next154 = or disjoint i64 %index149, 4
  %86 = getelementptr inbounds nuw double, ptr %18, i64 %index.next154
  %87 = getelementptr inbounds nuw i8, ptr %86, i64 16
  %wide.load150.1 = load <2 x double>, ptr %86, align 8, !tbaa !5, !alias.scope !35, !noalias !38
  %wide.load151.1 = load <2 x double>, ptr %87, align 8, !tbaa !5, !alias.scope !35, !noalias !38
  %88 = getelementptr inbounds nuw double, ptr %24, i64 %index.next154
  %89 = getelementptr inbounds nuw i8, ptr %88, i64 16
  %wide.load152.1 = load <2 x double>, ptr %88, align 8, !tbaa !5, !alias.scope !38
  %wide.load153.1 = load <2 x double>, ptr %89, align 8, !tbaa !5, !alias.scope !38
  %90 = fadd <2 x double> %wide.load150.1, %wide.load152.1
  %91 = fadd <2 x double> %wide.load151.1, %wide.load153.1
  store <2 x double> %90, ptr %86, align 8, !tbaa !5, !alias.scope !35, !noalias !38
  store <2 x double> %91, ptr %87, align 8, !tbaa !5, !alias.scope !35, !noalias !38
  %index.next154.1 = add nuw nsw i64 %index149, 8
  %92 = icmp eq i64 %index.next154.1, 2000
  br i1 %92, label %for.cond64.preheader.i.preheader, label %vector.body148, !llvm.loop !40

for.body50.i:                                     ; preds = %vector.memcheck140, %for.body50.i
  %indvars.iv157.i = phi i64 [ %indvars.iv.next158.i.3, %for.body50.i ], [ 0, %vector.memcheck140 ]
  %arrayidx52.i = getelementptr inbounds nuw double, ptr %18, i64 %indvars.iv157.i
  %93 = load double, ptr %arrayidx52.i, align 8, !tbaa !5
  %arrayidx54.i = getelementptr inbounds nuw double, ptr %24, i64 %indvars.iv157.i
  %94 = load double, ptr %arrayidx54.i, align 8, !tbaa !5
  %add55.i = fadd double %93, %94
  store double %add55.i, ptr %arrayidx52.i, align 8, !tbaa !5
  %indvars.iv.next158.i = or disjoint i64 %indvars.iv157.i, 1
  %arrayidx52.i.1 = getelementptr inbounds nuw double, ptr %18, i64 %indvars.iv.next158.i
  %95 = load double, ptr %arrayidx52.i.1, align 8, !tbaa !5
  %arrayidx54.i.1 = getelementptr inbounds nuw double, ptr %24, i64 %indvars.iv.next158.i
  %96 = load double, ptr %arrayidx54.i.1, align 8, !tbaa !5
  %add55.i.1 = fadd double %95, %96
  store double %add55.i.1, ptr %arrayidx52.i.1, align 8, !tbaa !5
  %indvars.iv.next158.i.1 = or disjoint i64 %indvars.iv157.i, 2
  %arrayidx52.i.2 = getelementptr inbounds nuw double, ptr %18, i64 %indvars.iv.next158.i.1
  %97 = load double, ptr %arrayidx52.i.2, align 8, !tbaa !5
  %arrayidx54.i.2 = getelementptr inbounds nuw double, ptr %24, i64 %indvars.iv.next158.i.1
  %98 = load double, ptr %arrayidx54.i.2, align 8, !tbaa !5
  %add55.i.2 = fadd double %97, %98
  store double %add55.i.2, ptr %arrayidx52.i.2, align 8, !tbaa !5
  %indvars.iv.next158.i.2 = or disjoint i64 %indvars.iv157.i, 3
  %arrayidx52.i.3 = getelementptr inbounds nuw double, ptr %18, i64 %indvars.iv.next158.i.2
  %99 = load double, ptr %arrayidx52.i.3, align 8, !tbaa !5
  %arrayidx54.i.3 = getelementptr inbounds nuw double, ptr %24, i64 %indvars.iv.next158.i.2
  %100 = load double, ptr %arrayidx54.i.3, align 8, !tbaa !5
  %add55.i.3 = fadd double %99, %100
  store double %add55.i.3, ptr %arrayidx52.i.3, align 8, !tbaa !5
  %indvars.iv.next158.i.3 = add nuw nsw i64 %indvars.iv157.i, 4
  %exitcond160.not.i.3 = icmp eq i64 %indvars.iv.next158.i.3, 2000
  br i1 %exitcond160.not.i.3, label %for.cond64.preheader.i.preheader, label %for.body50.i, !llvm.loop !41

for.cond64.preheader.i.preheader:                 ; preds = %vector.body148, %for.body50.i
  br label %for.cond64.preheader.i

for.cond64.preheader.i:                           ; preds = %for.cond64.preheader.i.preheader, %for.inc83.i
  %indvars.iv165.i = phi i64 [ %indvars.iv.next166.i, %for.inc83.i ], [ 0, %for.cond64.preheader.i.preheader ]
  %arrayidx68.i = getelementptr inbounds nuw double, ptr %15, i64 %indvars.iv165.i
  %arrayidx68.promoted.i = load double, ptr %arrayidx68.i, align 8, !tbaa !5
  br label %for.body66.i

for.body66.i:                                     ; preds = %for.body66.i, %for.cond64.preheader.i
  %indvars.iv161.i = phi i64 [ 0, %for.cond64.preheader.i ], [ %indvars.iv.next162.i.1, %for.body66.i ]
  %add77140141.i = phi double [ %arrayidx68.promoted.i, %for.cond64.preheader.i ], [ %add77.i.1, %for.body66.i ]
  %arrayidx72.i = getelementptr inbounds nuw [2000 x double], ptr %0, i64 %indvars.iv165.i, i64 %indvars.iv161.i
  %101 = load double, ptr %arrayidx72.i, align 8, !tbaa !5
  %mul73.i = fmul double %101, 1.500000e+00
  %arrayidx75.i = getelementptr inbounds nuw double, ptr %18, i64 %indvars.iv161.i
  %102 = load double, ptr %arrayidx75.i, align 8, !tbaa !5
  %mul76.i = fmul double %mul73.i, %102
  %add77.i = fadd double %add77140141.i, %mul76.i
  store double %add77.i, ptr %arrayidx68.i, align 8, !tbaa !5
  %indvars.iv.next162.i = or disjoint i64 %indvars.iv161.i, 1
  %arrayidx72.i.1 = getelementptr inbounds nuw [2000 x double], ptr %0, i64 %indvars.iv165.i, i64 %indvars.iv.next162.i
  %103 = load double, ptr %arrayidx72.i.1, align 8, !tbaa !5
  %mul73.i.1 = fmul double %103, 1.500000e+00
  %arrayidx75.i.1 = getelementptr inbounds nuw double, ptr %18, i64 %indvars.iv.next162.i
  %104 = load double, ptr %arrayidx75.i.1, align 8, !tbaa !5
  %mul76.i.1 = fmul double %mul73.i.1, %104
  %add77.i.1 = fadd double %add77.i, %mul76.i.1
  store double %add77.i.1, ptr %arrayidx68.i, align 8, !tbaa !5
  %indvars.iv.next162.i.1 = add nuw nsw i64 %indvars.iv161.i, 2
  %exitcond164.not.i.1 = icmp eq i64 %indvars.iv.next162.i.1, 2000
  br i1 %exitcond164.not.i.1, label %for.inc83.i, label %for.body66.i, !llvm.loop !42

for.inc83.i:                                      ; preds = %for.body66.i
  %indvars.iv.next166.i = add nuw nsw i64 %indvars.iv165.i, 1
  %exitcond168.not.i = icmp eq i64 %indvars.iv.next166.i, 2000
  br i1 %exitcond168.not.i, label %kernel_gemver.exit, label %for.cond64.preheader.i, !llvm.loop !43

kernel_gemver.exit:                               ; preds = %for.inc83.i
  call fastcc void @print_array(ptr noundef %15)
  call void @free(ptr noundef nonnull %0) #11
  call void @free(ptr noundef %3) #11
  call void @free(ptr noundef %6) #11
  call void @free(ptr noundef %9) #11
  call void @free(ptr noundef %12) #11
  call void @free(ptr noundef nonnull %15) #11
  call void @free(ptr noundef nonnull %18) #11
  call void @free(ptr noundef %21) #11
  call void @free(ptr noundef %24) #11
  ret i32 0
}

; Function Attrs: cold nofree nounwind uwtable
define internal fastcc void @print_array(ptr noundef nonnull readonly captures(none) %w) unnamed_addr #8 {
entry:
  %0 = load ptr, ptr @stderr, align 8, !tbaa !11
  %1 = tail call i64 @fwrite(ptr nonnull @.str.2, i64 22, i64 1, ptr %0) #12
  %2 = load ptr, ptr @stderr, align 8, !tbaa !11
  %call1 = tail call i32 (ptr, ptr, ...) @fprintf(ptr noundef %2, ptr noundef nonnull @.str.3, ptr noundef nonnull @.str.4) #14
  br label %for.body

for.body:                                         ; preds = %entry, %if.end
  %indvars.iv = phi i64 [ 0, %entry ], [ %indvars.iv.next, %if.end ]
  %rem.lhs.trunc = trunc i64 %indvars.iv to i16
  %rem10 = urem i16 %rem.lhs.trunc, 20
  %cmp2 = icmp eq i16 %rem10, 0
  br i1 %cmp2, label %if.then, label %if.end

if.then:                                          ; preds = %for.body
  %3 = load ptr, ptr @stderr, align 8, !tbaa !11
  %fputc = tail call i32 @fputc(i32 10, ptr %3)
  br label %if.end

if.end:                                           ; preds = %if.then, %for.body
  %4 = load ptr, ptr @stderr, align 8, !tbaa !11
  %arrayidx = getelementptr inbounds nuw double, ptr %w, i64 %indvars.iv
  %5 = load double, ptr %arrayidx, align 8, !tbaa !5
  %call4 = tail call i32 (ptr, ptr, ...) @fprintf(ptr noundef %4, ptr noundef nonnull @.str.6, double noundef %5) #14
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %exitcond.not = icmp eq i64 %indvars.iv.next, 2000
  br i1 %exitcond.not, label %for.end, label %for.body, !llvm.loop !44

for.end:                                          ; preds = %if.end
  %6 = load ptr, ptr @stderr, align 8, !tbaa !11
  %call5 = tail call i32 (ptr, ptr, ...) @fprintf(ptr noundef %6, ptr noundef nonnull @.str.7, ptr noundef nonnull @.str.4) #14
  %7 = load ptr, ptr @stderr, align 8, !tbaa !11
  %8 = tail call i64 @fwrite(ptr nonnull @.str.8, i64 22, i64 1, ptr %7) #12
  ret void
}

; Function Attrs: nofree nounwind
declare i32 @posix_memalign(ptr noundef, i64 noundef, i64 noundef) local_unnamed_addr #5

; Function Attrs: nofree nounwind
declare noundef i32 @fprintf(ptr noundef captures(none), ptr noundef readonly captures(none), ...) local_unnamed_addr #5

; Function Attrs: nofree noreturn nounwind
declare void @exit(i32 noundef) local_unnamed_addr #9

; Function Attrs: nofree nounwind
declare noundef i64 @fwrite(ptr noundef readonly captures(none), i64 noundef, i64 noundef, ptr noundef captures(none)) local_unnamed_addr #10

; Function Attrs: nofree nounwind
declare noundef i32 @fputc(i32 noundef, ptr noundef captures(none)) local_unnamed_addr #10

attributes #0 = { mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }
attributes #2 = { mustprogress nounwind willreturn allockind("free") memory(argmem: readwrite, inaccessiblemem: readwrite) "alloc-family"="malloc" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #3 = { mustprogress nofree norecurse nosync nounwind willreturn memory(write, argmem: none, inaccessiblemem: none) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #4 = { nofree nounwind uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #5 = { nofree nounwind "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #6 = { mustprogress nounwind willreturn memory(argmem: readwrite, inaccessiblemem: readwrite) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #7 = { nounwind uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #8 = { cold nofree nounwind uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #9 = { nofree noreturn nounwind "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #10 = { nofree nounwind }
attributes #11 = { nounwind }
attributes #12 = { cold }
attributes #13 = { cold noreturn nounwind }
attributes #14 = { cold nounwind }

!llvm.module.flags = !{!0, !1, !2, !3}
!llvm.ident = !{!4}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 8, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{!"clang version 21.1.8 (/root/yansollvm/work/llvm-project-21.1.8.src/clang 74dd0a496a77bb5265f8fc1f62c6ba9a60522449)"}
!5 = !{!6, !6, i64 0}
!6 = !{!"double", !7, i64 0}
!7 = !{!"omnipotent char", !8, i64 0}
!8 = !{!"Simple C/C++ TBAA"}
!9 = !{!10, !10, i64 0}
!10 = !{!"any pointer", !7, i64 0}
!11 = !{!12, !12, i64 0}
!12 = !{!"p1 _ZTS8_IO_FILE", !10, i64 0}
!13 = distinct !{!13, !14, !15, !16}
!14 = !{!"llvm.loop.mustprogress"}
!15 = !{!"llvm.loop.isvectorized", i32 1}
!16 = !{!"llvm.loop.unroll.runtime.disable"}
!17 = distinct !{!17, !14}
!18 = !{!19}
!19 = distinct !{!19, !20}
!20 = distinct !{!20, !"LVerDomain"}
!21 = !{!22}
!22 = distinct !{!22, !20}
!23 = !{!24}
!24 = distinct !{!24, !20}
!25 = !{!19, !26, !22, !27}
!26 = distinct !{!26, !20}
!27 = distinct !{!27, !20}
!28 = !{!26}
!29 = !{!27}
!30 = distinct !{!30, !14, !15, !16}
!31 = distinct !{!31, !14, !15}
!32 = distinct !{!32, !14}
!33 = distinct !{!33, !14}
!34 = distinct !{!34, !14}
!35 = !{!36}
!36 = distinct !{!36, !37}
!37 = distinct !{!37, !"LVerDomain"}
!38 = !{!39}
!39 = distinct !{!39, !37}
!40 = distinct !{!40, !14, !15, !16}
!41 = distinct !{!41, !14, !15}
!42 = distinct !{!42, !14}
!43 = distinct !{!43, !14}
!44 = distinct !{!44, !14}
