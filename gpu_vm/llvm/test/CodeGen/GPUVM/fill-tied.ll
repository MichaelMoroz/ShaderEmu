; RUN: llc -mtriple=gpuvm -O2 -relocation-model=pic -regalloc-csr-first-time-cost=65536 < %s | FileCheck %s
; Half-Life's CL_SetLightstyle (Xash3D's cl_tent.c, the PGO build's IR). Its second loop is counted by addbnz,
; whose counter is a use tied to the def. GlobalFill renamed the counter's register in the blocks after its maxi
; (moved into each way in) but not the def tied to that use, so the loop's other exit compared a stale register
; (beq t7, t3 with t3 the string's pointer): 2^32 rounds, and the game hung loading on the card.

; CHECK-LABEL: CL_SetLightstyle:
; CHECK: addbnz [[C:t[0-9]+]], -1,
; CHECK: beq {{t[0-9]+}}, [[C]],
; CHECK: beq {{t[0-9]+}}, [[C]],
target datalayout = "e-m:e-p:32:32-i64:64-n32-S128"
target triple = "gpuvm-unknown-linux"

%struct.client_t = type { double, double, i32, i32, i32, i32, [1025 x ptr], i32, [3 x float], i32, i32, i32, i32, i32, i32, i32, [2 x double], float, i32, i32, i32, i32, i32, i32, i32, [64 x %struct.frame_s], [64 x %struct.runcmd_s], [64 x %struct.local_state_s], double, [512 x i8], [32 x %struct.player_info_s], double, i32, %struct.event_state_s, %struct.cl_local_data_t, %struct.usercmd_s, [3 x float], [3 x float], [3 x float], i32, [3 x float], [16 x %struct.pred_viewangle_t], i32, float, float, [3 x float], [8 x %struct.entity_state_s], i32, [2048 x [48 x i8]], [128 x [48 x i8]], [512 x [48 x i8]], [256 x %struct.lightstyle_t], i32, [1024 x %struct.consistency_s], i32, i32, %struct.resource_s, %struct.resource_s, [1024 x %struct.resource_s], i32, i32, [16 x i8], [2048 x i16], [512 x i16], ptr, i32, double, i32 }
%struct.frame_s = type { double, double, double, i32, i32, %struct.clientdata_s, [32 x %struct.entity_state_s], [64 x %struct.weapon_data_s], %struct.netbandwithgraph_s, [32 x i8], i32, i32 }
%struct.clientdata_s = type { [3 x float], [3 x float], i32, [3 x float], i32, i32, i32, [3 x float], float, i32, i32, i32, i32, i32, i32, float, float, i32, i32, i32, i32, i32, i32, float, i32, i32, i32, [256 x i8], i32, i32, i32, i32, float, float, float, float, [3 x float], [3 x float], [3 x float], [3 x float] }
%struct.entity_state_s = type { i32, i32, float, i32, [3 x float], [3 x float], i32, i32, float, i32, i16, i16, i32, float, i8, i32, i32, %struct.color24, i32, i32, float, float, i32, [4 x i8], [4 x i8], [3 x float], [3 x float], [3 x float], i32, i32, float, float, i32, i32, i32, i32, i32, i32, [3 x float], i32, i32, i32, i32, float, float, i32, [3 x float], [3 x float], float, float, i32, i32, i32, i32, float, float, float, float, [3 x float], [3 x float], [3 x float], [3 x float] }
%struct.color24 = type { i8, i8, i8 }
%struct.weapon_data_s = type { i32, i32, float, float, float, i32, i32, float, float, float, float, float, i32, i32, i32, i32, i32, i32, float, float, float, float }
%struct.netbandwithgraph_s = type { i16, i16, i16, i16, i16, i16, i16, i16, i16 }
%struct.runcmd_s = type { double, double, float, %struct.usercmd_s, i32, i32, i32 }
%struct.local_state_s = type { %struct.entity_state_s, %struct.clientdata_s, [64 x %struct.weapon_data_s] }
%struct.player_info_s = type { i32, [256 x i8], [32 x i8], i32, i32, i32, [64 x i8], i32, i32, i32, i32, float, float, [3 x float], %struct.customization_s, [16 x i8] }
%struct.customization_s = type { i32, %struct.resource_s, i32, i32, i32, ptr, ptr, ptr }
%struct.event_state_s = type { [64 x %struct.event_info_s] }
%struct.event_info_s = type { i16, i16, i16, float, %struct.event_args_s, i32 }
%struct.event_args_s = type { i32, i32, [3 x float], [3 x float], [3 x float], i32, float, float, i32, i32, i32, i32 }
%struct.cl_local_data_t = type { [64 x [3 x float]], [3 x float], [3 x float], i32, float, i32, i32, float, i32, i32, i32, i32, i32, i32, i32, i32, i32, float, float, i32, float }
%struct.usercmd_s = type { i16, i8, i8, [3 x float], float, float, float, i8, i8, i16, i8, i8, [2 x i8], [4 x i32] }
%struct.pred_viewangle_t = type { float, float }
%struct.lightstyle_t = type { [256 x i8], [256 x float], i32, float, i32, float }
%struct.consistency_s = type { ptr, i32, i32, i32, i32, [3 x float], [3 x float] }
%struct.resource_s = type { [64 x i8], i32, i32, i32, i8, [16 x i8], i8, [32 x i8], i16, ptr, ptr }

@cl = external global %struct.client_t, align 8
@.str.12 = external hidden unnamed_addr constant [51 x i8], align 1
@__func__.CL_SetLightstyle = external hidden unnamed_addr constant [17 x i8], align 1
@.str.13 = external hidden unnamed_addr constant [31 x i8], align 1
@.str.14 = external hidden unnamed_addr constant [4 x i8], align 1
@.str.15 = external hidden unnamed_addr constant [3 x i8], align 1

; Function Attrs: denormal_fpenv(float: preservesign)
declare void @Con_Reportf(ptr noundef, ...) local_unnamed_addr #0

; Function Attrs: denormal_fpenv(float: preservesign)
declare void @Con_Printf(ptr noundef, ...) local_unnamed_addr #0

; Function Attrs: nounwind denormal_fpenv(float: preservesign)
define dso_local void @CL_SetLightstyle(i32 noundef %style, ptr noundef %s, float noundef %f) local_unnamed_addr #1 !prof !38 {
entry:
  %0 = icmp ugt i32 %style, 255
  br i1 %0, label %if.then, label %if.end, !prof !39

if.then:                                          ; preds = %entry
  tail call void (ptr, ...) @Con_Printf(ptr noundef nonnull @.str.12, ptr noundef nonnull @__func__.CL_SetLightstyle, i32 noundef %style) #4
  br label %return

if.end:                                           ; preds = %entry
  %arrayidx = getelementptr inbounds nuw [1296 x i8], ptr getelementptr inbounds nuw (i8, ptr @cl, i32 1673368), i32 %style
  %tobool1.i = icmp eq ptr %s, null
  br i1 %tobool1.i, label %Q_strncpy.exit.thread, label %Q_strncpy.exit, !prof !39

Q_strncpy.exit.thread:                            ; preds = %if.end
  %length80 = getelementptr inbounds nuw i8, ptr %arrayidx, i32 1280
  store i32 0, ptr %length80, align 8
  %time81 = getelementptr inbounds nuw i8, ptr %arrayidx, i32 1292
  store float %f, ptr %time81, align 4
  br label %for.cond.cleanup

Q_strncpy.exit:                                   ; preds = %if.end
  %call.i = tail call i32 @strlcpy(ptr noundef nonnull dereferenceable(1) %arrayidx, ptr noundef nonnull dereferenceable(1) %s, i32 noundef 256) #4, !prof !40
  %length = getelementptr inbounds nuw i8, ptr %arrayidx, i32 1280
  store i32 %call.i, ptr %length, align 8
  %time = getelementptr inbounds nuw i8, ptr %arrayidx, i32 1292
  store float %f, ptr %time, align 4
  %cmp376 = icmp sgt i32 %call.i, 0
  br i1 %cmp376, label %for.body.lr.ph, label %for.cond.cleanup, !prof !41

for.body.lr.ph:                                   ; preds = %Q_strncpy.exit
  %map = getelementptr inbounds nuw i8, ptr %arrayidx, i32 256
  br label %for.body

for.cond.cleanup:                                 ; preds = %for.body.1, %for.body, %Q_strncpy.exit, %Q_strncpy.exit.thread
  %1 = phi i32 [ %call.i, %Q_strncpy.exit ], [ 0, %Q_strncpy.exit.thread ], [ %4, %for.body ], [ %6, %for.body.1 ]
  %cmp8 = icmp sgt i32 %1, 1
  %cond = zext i1 %cmp8 to i32
  %interp = getelementptr inbounds nuw i8, ptr %arrayidx, i32 1288
  store i32 %cond, ptr %interp, align 8
  %map18 = getelementptr inbounds nuw i8, ptr %arrayidx, i32 256
  %2 = tail call i32 @llvm.smax.i32(i32 %1, i32 1)
  %smax = add nsw i32 %2, -1
  %exitcond.not83 = icmp eq i32 %smax, 0
  br i1 %exitcond.not83, label %cleanup37.loopexit, label %for.body17, !prof !42

for.body:                                         ; preds = %for.body.1, %for.body.lr.ph
  %i.077 = phi i32 [ 0, %for.body.lr.ph ], [ %inc.1, %for.body.1 ]
  %arrayidx4 = getelementptr inbounds nuw i8, ptr %s, i32 %i.077
  %3 = load i8, ptr %arrayidx4, align 1
  %conv = sext i8 %3 to i32
  %sub = add nsw i32 %conv, -97
  %conv5 = sitofp contract afn i32 %sub to float
  %arrayidx6 = getelementptr inbounds nuw [4 x i8], ptr %map, i32 %i.077
  store float %conv5, ptr %arrayidx6, align 8
  %inc = or disjoint i32 %i.077, 1
  %4 = load i32, ptr %length, align 8
  %cmp3 = icmp slt i32 %inc, %4
  br i1 %cmp3, label %for.body.1, label %for.cond.cleanup, !prof !43

for.body.1:                                       ; preds = %for.body
  %arrayidx4.1 = getelementptr inbounds nuw i8, ptr %s, i32 %inc
  %5 = load i8, ptr %arrayidx4.1, align 1
  %conv.1 = sext i8 %5 to i32
  %sub.1 = add nsw i32 %conv.1, -97
  %conv5.1 = sitofp contract afn i32 %sub.1 to float
  %arrayidx6.1 = getelementptr inbounds nuw [4 x i8], ptr %map, i32 %inc
  store float %conv5.1, ptr %arrayidx6.1, align 4
  %inc.1 = add nuw nsw i32 %i.077, 2
  %6 = load i32, ptr %length, align 8
  %cmp3.1 = icmp slt i32 %inc.1, %6
  br i1 %cmp3.1, label %for.body, label %for.cond.cleanup, !prof !43, !llvm.loop !44

for.cond11:                                       ; preds = %for.body17
  %exitcond.not = icmp eq i32 %add22, %smax
  br i1 %exitcond.not, label %cleanup37.loopexit, label %for.body17.1, !prof !48

for.body17.1:                                     ; preds = %for.cond11
  %rem.1 = srem i32 %add22, %1
  %arrayidx20.1 = getelementptr inbounds nuw [4 x i8], ptr %map18, i32 %rem.1
  %7 = load float, ptr %arrayidx20.1, align 4
  %add22.1 = add nuw nsw i32 %i10.084, 2
  %rem24.1 = srem i32 %add22.1, %1
  %arrayidx25.1 = getelementptr inbounds nuw [4 x i8], ptr %map18, i32 %rem24.1
  %8 = load float, ptr %arrayidx25.1, align 4
  %sub26.1 = fsub contract afn float %7, %8
  %9 = tail call contract afn float @llvm.fabs.f32(float %sub26.1)
  %cmp29.1 = fcmp contract afn ule float %9, 3.000000e+00
  br i1 %cmp29.1, label %for.cond11.1, label %if.then31, !prof !49, !llvm.loop !50

for.cond11.1:                                     ; preds = %for.body17.1
  %exitcond.not.1 = icmp eq i32 %add22.1, %smax
  br i1 %exitcond.not.1, label %cleanup37.loopexit, label %for.body17, !prof !48, !llvm.loop !52

for.body17:                                       ; preds = %for.cond11.1, %for.cond.cleanup
  %i10.084 = phi i32 [ %add22.1, %for.cond11.1 ], [ 0, %for.cond.cleanup ]
  %rem = srem i32 %i10.084, %1
  %arrayidx20 = getelementptr inbounds nuw [4 x i8], ptr %map18, i32 %rem
  %10 = load float, ptr %arrayidx20, align 4
  %add22 = or disjoint i32 %i10.084, 1
  %rem24 = srem i32 %add22, %1
  %arrayidx25 = getelementptr inbounds nuw [4 x i8], ptr %map18, i32 %rem24
  %11 = load float, ptr %arrayidx25, align 4
  %sub26 = fsub contract afn float %10, %11
  %12 = tail call contract afn float @llvm.fabs.f32(float %sub26)
  %cmp29 = fcmp contract afn ule float %12, 3.000000e+00
  br i1 %cmp29, label %for.cond11, label %if.then31, !prof !49, !llvm.loop !50

if.then31:                                        ; preds = %for.body17, %for.body17.1
  store i32 0, ptr %interp, align 8
  br label %cleanup37

cleanup37.loopexit:                               ; preds = %for.cond11.1, %for.cond11, %for.cond.cleanup
  %13 = select i1 %cmp8, ptr @.str.14, ptr @.str.15
  br label %cleanup37

cleanup37:                                        ; preds = %cleanup37.loopexit, %if.then31
  %tobool46.not = phi ptr [ %13, %cleanup37.loopexit ], [ @.str.15, %if.then31 ]
  %cmp40 = icmp sgt i32 %1, 0
  br i1 %cmp40, label %if.then42, label %return, !prof !53

if.then42:                                        ; preds = %cleanup37
  tail call void (ptr, ...) @Con_Reportf(ptr noundef nonnull @.str.13, i32 noundef %style, ptr noundef nonnull %arrayidx, ptr noundef nonnull %tobool46.not) #4, !prof !40
  br label %return

return:                                           ; preds = %if.then42, %cleanup37, %if.then
  ret void
}

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare float @llvm.fabs.f32(float) #2

; Function Attrs: nofree denormal_fpenv(float: preservesign)
declare i32 @strlcpy(ptr noundef, ptr noundef, i32 noundef) local_unnamed_addr #3

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare i32 @llvm.smax.i32(i32, i32) #2

attributes #0 = { denormal_fpenv(float: preservesign) "no-trapping-math"="true" "stack-protector-buffer-size"="8" }
attributes #1 = { nounwind denormal_fpenv(float: preservesign) "no-trapping-math"="true" "stack-protector-buffer-size"="8" "use-sample-profile" }
attributes #2 = { nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }
attributes #3 = { nofree denormal_fpenv(float: preservesign) "no-trapping-math"="true" "stack-protector-buffer-size"="8" }
attributes #4 = { nounwind }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !33}
!llvm.ident = !{!37}

!0 = !{i32 7, !"Dwarf Version", i32 5}
!1 = !{i32 2, !"Debug Info Version", i32 3}
!2 = !{i32 8, !"PIC Level", i32 2}
!3 = !{i32 7, !"PIE Level", i32 2}
!4 = !{i32 1, !"ProfileSummary", !5}
!5 = !{!6, !7, !8, !9, !10, !11, !12, !13, !14, !15}
!6 = !{!"ProfileFormat", !"SampleProfile"}
!7 = !{!"TotalCount", i64 296952822}
!8 = !{!"MaxCount", i64 6700529}
!9 = !{!"MaxInternalCount", i64 0}
!10 = !{!"MaxFunctionCount", i64 540039}
!11 = !{!"NumCounts", i64 13678}
!12 = !{!"NumFunctions", i64 958}
!13 = !{!"IsPartialProfile", i64 0}
!14 = !{!"PartialProfileRatio", double 0.000000e+00}
!15 = !{!"DetailedSummary", !16}
!16 = !{!17, !18, !19, !20, !21, !22, !23, !24, !25, !26, !27, !28, !29, !30, !31, !32}
!17 = !{i32 10000, i64 6700529, i32 2}
!18 = !{i32 100000, i64 1958092, i32 10}
!19 = !{i32 200000, i64 1852453, i32 26}
!20 = !{i32 300000, i64 794419, i32 47}
!21 = !{i32 400000, i64 392586, i32 105}
!22 = !{i32 500000, i64 233472, i32 202}
!23 = !{i32 600000, i64 160551, i32 350}
!24 = !{i32 700000, i64 100320, i32 587}
!25 = !{i32 800000, i64 36068, i32 1163}
!26 = !{i32 900000, i64 24335, i32 2058}
!27 = !{i32 950000, i64 10032, i32 2935}
!28 = !{i32 990000, i64 912, i32 9496}
!29 = !{i32 999000, i64 726, i32 10503}
!30 = !{i32 999900, i64 56, i32 11520}
!31 = !{i32 999990, i64 8, i32 12700}
!32 = !{i32 999999, i64 2, i32 13419}
!33 = !{i32 5, !"CG Profile", !34}
!34 = distinct !{!35, !36}
!35 = !{ptr @CL_SetLightstyle, ptr @strlcpy, i64 5}
!36 = !{ptr @CL_SetLightstyle, ptr @Con_Reportf, i64 5}
!37 = !{!"clang version 23.1.4 (https://github.com/llvm/llvm-project.git 7a941dfb2877a0b8cf30323fefa85ad019e0b0f5)"}
!38 = !{!"function_entry_count", i64 5}
!39 = !{!"branch_weights", !"expected", i32 1, i32 2000}
!40 = !{!"branch_weights", i32 5}
!41 = !{!"branch_weights", i32 2135128906, i32 12354742}
!42 = !{!"branch_weights", i32 4, i32 1}
!43 = !{!"branch_weights", i32 97, i32 159}
!44 = distinct !{!44, !45, !46, !47}
!45 = !{!"llvm.loop.mustprogress"}
!46 = !{!"llvm.loop.estimated_trip_count", i32 1}
!47 = !{!"llvm.loop.unroll.disable"}
!48 = !{!"branch_weights", i32 1, i32 0}
!49 = !{!"branch_weights", i32 0, i32 1}
!50 = distinct !{!50, !45, !51}
!51 = !{!"llvm.loop.unroll.count", i32 2}
!52 = distinct !{!52, !45, !46, !47}
!53 = !{!"branch_weights", i32 5, i32 0}
