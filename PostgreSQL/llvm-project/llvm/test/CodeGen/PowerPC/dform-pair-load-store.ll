; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc -verify-machineinstrs -mtriple=powerpc64le-unknown-linux-gnu \
; RUN:   -mcpu=pwr10 -ppc-asm-full-reg-names < %s | FileCheck %s
; RUN: llc -verify-machineinstrs -mtriple=powerpc64-unknown-linux-gnu \
; RUN:   -mcpu=pwr10 -ppc-asm-full-reg-names < %s | FileCheck %s \
; RUN:   --check-prefix=CHECK-BE

; This test checks that LSR properly recognizes lxvp/stxvp as load/store
; intrinsics to avoid generating x-form instructions instead of d-forms.

declare <256 x i1> @llvm.ppc.vsx.lxvp(i8*)
declare void @llvm.ppc.vsx.stxvp(<256 x i1>, i8*)
define void @foo(i32 zeroext %n, <256 x i1>* %ptr, <256 x i1>* %ptr2) {
; CHECK-LABEL: foo:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    cmplwi r3, 0
; CHECK-NEXT:    beqlr cr0
; CHECK-NEXT:  # %bb.1: # %for.body.lr.ph
; CHECK-NEXT:    clrldi r6, r3, 32
; CHECK-NEXT:    addi r3, r4, 64
; CHECK-NEXT:    addi r4, r5, 64
; CHECK-NEXT:    mtctr r6
; CHECK-NEXT:    .p2align 4
; CHECK-NEXT:  .LBB0_2: # %for.body
; CHECK-NEXT:    #
; CHECK-NEXT:    lxvp vsp34, -64(r3)
; CHECK-NEXT:    lxvp vsp36, -32(r3)
; CHECK-NEXT:    lxvp vsp32, 0(r3)
; CHECK-NEXT:    lxvp vsp38, 32(r3)
; CHECK-NEXT:    addi r3, r3, 1
; CHECK-NEXT:    stxvp vsp34, -64(r4)
; CHECK-NEXT:    stxvp vsp36, -32(r4)
; CHECK-NEXT:    stxvp vsp32, 0(r4)
; CHECK-NEXT:    stxvp vsp38, 32(r4)
; CHECK-NEXT:    addi r4, r4, 1
; CHECK-NEXT:    bdnz .LBB0_2
; CHECK-NEXT:  # %bb.3: # %for.cond.cleanup
; CHECK-NEXT:    blr
;
; CHECK-BE-LABEL: foo:
; CHECK-BE:       # %bb.0: # %entry
; CHECK-BE-NEXT:    cmplwi r3, 0
; CHECK-BE-NEXT:    beqlr cr0
; CHECK-BE-NEXT:  # %bb.1: # %for.body.lr.ph
; CHECK-BE-NEXT:    clrldi r6, r3, 32
; CHECK-BE-NEXT:    addi r3, r4, 64
; CHECK-BE-NEXT:    addi r4, r5, 64
; CHECK-BE-NEXT:    mtctr r6
; CHECK-BE-NEXT:    .p2align 4
; CHECK-BE-NEXT:  .LBB0_2: # %for.body
; CHECK-BE-NEXT:    #
; CHECK-BE-NEXT:    lxvp vsp34, -64(r3)
; CHECK-BE-NEXT:    lxvp vsp36, -32(r3)
; CHECK-BE-NEXT:    lxvp vsp32, 0(r3)
; CHECK-BE-NEXT:    lxvp vsp38, 32(r3)
; CHECK-BE-NEXT:    addi r3, r3, 1
; CHECK-BE-NEXT:    stxvp vsp34, -64(r4)
; CHECK-BE-NEXT:    stxvp vsp36, -32(r4)
; CHECK-BE-NEXT:    stxvp vsp32, 0(r4)
; CHECK-BE-NEXT:    stxvp vsp38, 32(r4)
; CHECK-BE-NEXT:    addi r4, r4, 1
; CHECK-BE-NEXT:    bdnz .LBB0_2
; CHECK-BE-NEXT:  # %bb.3: # %for.cond.cleanup
; CHECK-BE-NEXT:    blr
entry:
  %cmp35.not = icmp eq i32 %n, 0
  br i1 %cmp35.not, label %for.cond.cleanup, label %for.body.lr.ph

for.body.lr.ph:
  %0 = bitcast <256 x i1>* %ptr to i8*
  %1 = bitcast <256 x i1>* %ptr2 to i8*
  %wide.trip.count = zext i32 %n to i64
  br label %for.body

for.cond.cleanup:
  ret void

for.body:
  %indvars.iv = phi i64 [ 0, %for.body.lr.ph ], [ %indvars.iv.next, %for.body ]
  %2 = getelementptr i8, i8* %0, i64 %indvars.iv
  %3 = tail call <256 x i1> @llvm.ppc.vsx.lxvp(i8* %2)
  %add2 = add nuw nsw i64 %indvars.iv, 32
  %4 = getelementptr i8, i8* %0, i64 %add2
  %5 = tail call <256 x i1> @llvm.ppc.vsx.lxvp(i8* %4)
  %add4 = add nuw nsw i64 %indvars.iv, 64
  %6 = getelementptr i8, i8* %0, i64 %add4
  %7 = tail call <256 x i1> @llvm.ppc.vsx.lxvp(i8* %6)
  %add6 = add nuw nsw i64 %indvars.iv, 96
  %8 = getelementptr i8, i8* %0, i64 %add6
  %9 = tail call <256 x i1> @llvm.ppc.vsx.lxvp(i8* %8)
  %10 = getelementptr i8, i8* %1, i64 %indvars.iv
  tail call void @llvm.ppc.vsx.stxvp(<256 x i1> %3, i8* %10)
  %11 = getelementptr i8, i8* %1, i64 %add2
  tail call void @llvm.ppc.vsx.stxvp(<256 x i1> %5, i8* %11)
  %12 = getelementptr i8, i8* %1, i64 %add4
  tail call void @llvm.ppc.vsx.stxvp(<256 x i1> %7, i8* %12)
  %13 = getelementptr i8, i8* %1, i64 %add6
  tail call void @llvm.ppc.vsx.stxvp(<256 x i1> %9, i8* %13)
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %exitcond.not = icmp eq i64 %indvars.iv.next, %wide.trip.count
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body
}
