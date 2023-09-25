/*
 * Copyright (c) 2020, 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "oops/oop.hpp"
#include "opto/c2_CodeStubs.hpp"
#include "opto/c2_MacroAssembler.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/lockStack.hpp"
#include "runtime/objectMonitor.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/sizes.hpp"

#define __ masm.

int C2SafepointPollStub::max_size() const {
  return 33;
}

void C2SafepointPollStub::emit(C2_MacroAssembler& masm) {
  assert(SharedRuntime::polling_page_return_handler_blob() != nullptr,
         "polling page return stub not created yet");
  address stub = SharedRuntime::polling_page_return_handler_blob()->entry_point();

  RuntimeAddress callback_addr(stub);

  __ bind(entry());
  InternalAddress safepoint_pc(masm.pc() - masm.offset() + _safepoint_offset);
#ifdef _LP64
  __ lea(rscratch1, safepoint_pc);
  __ movptr(Address(r15_thread, JavaThread::saved_exception_pc_offset()), rscratch1);
#else
  const Register tmp1 = rcx;
  const Register tmp2 = rdx;
  __ push(tmp1);
  __ push(tmp2);

  __ lea(tmp1, safepoint_pc);
  __ get_thread(tmp2);
  __ movptr(Address(tmp2, JavaThread::saved_exception_pc_offset()), tmp1);

  __ pop(tmp2);
  __ pop(tmp1);
#endif
  __ jump(callback_addr);
}

int C2EntryBarrierStub::max_size() const {
  return 10;
}

void C2EntryBarrierStub::emit(C2_MacroAssembler& masm) {
  __ bind(entry());
  __ call(RuntimeAddress(StubRoutines::method_entry_barrier()));
  __ jmp(continuation(), false /* maybe_short */);
}

int C2FastUnlockLightweightStub::max_size() const {
  return 128;
}

void C2FastUnlockLightweightStub::emit(C2_MacroAssembler& masm) {
  assert(_t == rax, "must be");

  Label restore_held_monitor_count_and_slow_path;

  { // Restore lock-stack and handle the unlock in runtime.

    __ bind(_push_and_slow_path);
#ifdef ASSERT
    // The obj was only cleared in debug.
    __ movl(_t, Address(_thread, JavaThread::lock_stack_top_offset()));
    __ movptr(Address(_thread, _t), _obj);
#endif
    __ addl(Address(_thread, JavaThread::lock_stack_top_offset()), oopSize);
  }

  { // Restore held monitor and slow path.

    __ bind(restore_held_monitor_count_and_slow_path);
    // Restore held monitor count.
    __ increment(Address(_thread, JavaThread::held_monitor_count_offset()));
    // increment will always result in ZF = 0 (no overflows).
    // continuation is the slow_path.
    __ jmp(continuation());
  }

  { // Handle monitor medium path.

    __ bind(_check_successor);

    Label fix_zf_and_unlocked;
    const Register monitor = _mark;

#ifndef _LP64
    // The owner may be anonymous, see comment in x86_64 section.
    __ movptr(Address(monitor, OM_OFFSET_NO_MONITOR_VALUE_TAG(owner)), _thread);
    __ jmpb(restore_held_monitor_count_and_slow_path);
#else // _LP64
    // The owner may be anonymous and we removed the last obj entry in
    // the lock-stack. This loses the information about the owner.
    // Write the thread to the owner field so the runtime knows the owner.
    __ movptr(Address(monitor, OM_OFFSET_NO_MONITOR_VALUE_TAG(owner)), _thread);

    // successor null check.
    __ cmpptr(Address(monitor, OM_OFFSET_NO_MONITOR_VALUE_TAG(succ)), NULL_WORD);
    __ jccb(Assembler::equal, restore_held_monitor_count_and_slow_path);

    // Release lock.
    __ movptr(Address(monitor, OM_OFFSET_NO_MONITOR_VALUE_TAG(owner)), NULL_WORD);

    // Fence.
    __ lock(); __ addl(Address(rsp, 0), 0);

    // Recheck successor.
    __ cmpptr(Address(monitor, OM_OFFSET_NO_MONITOR_VALUE_TAG(succ)), NULL_WORD);
    // Seen a successor after the release -> fence we have handed of the monitor
    __ jccb(Assembler::notEqual, fix_zf_and_unlocked);

    // Try to relock, if it fail the monitor has been handed over
    // TODO: Caveat, this may fail due to deflation, which does
    //       not handle the monitor handoff. Currently only works
    //       due to the responsible thread.
    __ xorptr(rax, rax);
    __ lock(); __ cmpxchgptr(_thread, Address(monitor, OM_OFFSET_NO_MONITOR_VALUE_TAG(owner)));
    __ jccb  (Assembler::equal, restore_held_monitor_count_and_slow_path);
#endif

    __ bind(fix_zf_and_unlocked);
    __ xorl(rax, rax);
    __ jmp(unlocked());
  }
}

#ifdef _LP64
int C2LightweightRecursiveLockStub::max_size() const {
  return DEBUG_ONLY(102) NOT_DEBUG(57);
}

void C2LightweightRecursiveLockStub::emit(C2_MacroAssembler& masm) {
#ifdef ASSERT
  Label check_zf_zero, check_zf_one;
  __ bind(check_zf_zero);
  __ jcc(Assembler::notZero, continuation());
  __ stop("check_zf_zero failed");
  __ bind(check_zf_one);
  __ jcc(Assembler::zero, continuation());
  __ stop("check_zf_one failed");
#endif

  Label& zf_zero = DEBUG_ONLY(check_zf_zero) NOT_DEBUG(continuation());
  Label& zf_one = DEBUG_ONLY(check_zf_one) NOT_DEBUG(continuation());

  Label found, loop;
  Register obj = object();
  Register t = tmp();

  __ bind(entry());

  // Load base offset, displace the offset by one entry so we can use jump if greater with ZF == 0
  int entry_displacement = oopSize;
  __ movq(t, in_bytes(JavaThread::lock_stack_base_offset()) + entry_displacement);
  __ bind(loop);
  __ cmpl(t, Address(r15_thread, JavaThread::lock_stack_top_offset()));
  // jump out if t > _top, so ZF == 0 here // FAIL
  __ jcc(Assembler::greater, zf_zero);
  // Check oop
  __ cmpq(obj, Address(r15_thread, t, Address::times_1, -entry_displacement));
  __ jccb(Assembler::equal, found);
  __ increment(t, oopSize);
  __ jmpb(loop);


  __ bind(found);
  __ movbool(Address(r15_thread, JavaThread::lock_stack_has_recu_offset()), true);

  int recu_displacement = LockStack::CAPACITY * oopSize;
  // t = LockStack::_base[N] + entry_displacement offset in thread,
  // add recu_displacement - entry_displacement
  // to get LockStack::_recu[N] offset in thread
  // oopSize == sizeof(size_t)
  __ increment(t, -entry_displacement + recu_displacement);
  __ increment(Address(r15_thread, t));
  // Set ZF == 1
  __ xorq(t, t);
  // jump out with ZF == 1 here // SUCC
  __ jmp(zf_one);
}

int C2LightweightRecursiveUnlockStub::max_size() const {
  return DEBUG_ONLY(256) NOT_DEBUG(200);
}

void C2LightweightRecursiveUnlockStub::emit(C2_MacroAssembler& masm) {
#ifdef ASSERT
  Label check_zf_zero, check_zf_one;
  __ bind(check_zf_zero);
  __ jcc(Assembler::notZero, continuation());
  __ stop("check_zf_zero failed");
  __ bind(check_zf_one);
  __ jcc(Assembler::zero, continuation());
  __ stop("check_zf_one failed");
#endif

  Label& zf_zero = DEBUG_ONLY(check_zf_zero) NOT_DEBUG(continuation());
  Label& zf_one = DEBUG_ONLY(check_zf_one) NOT_DEBUG(continuation());

  Label found, loop, loop_found, fix_lock_stack, fix_has_recu, set_zf_one;
  Register obj = object();
  Register t = tmp1();
  Register has_recu = tmp2();

  __ bind(entry());

  // Set has_recu = 0
  __ xorq(has_recu, has_recu);

  int recu_displacement = LockStack::CAPACITY * oopSize;
  // Load base offset, displace the offset by one entry so we can use jump if greater with ZF == 0
  int entry_displacement = oopSize;
  __ movl(t, in_bytes(JavaThread::lock_stack_base_offset()) + entry_displacement);

  __ bind(loop);
  __ cmpl(t, Address(r15_thread, JavaThread::lock_stack_top_offset()));
  // jump out if t > _top, so ZF == 0 here // FAIL
  __ jcc(Assembler::greater, zf_zero);
  // Check oop
  __ cmpptr(obj, Address(r15_thread, t, Address::times_1, -entry_displacement));
  __ jccb(Assembler::equal, found);
  // Check for other recursions
  __ orq(has_recu, Address(r15_thread, t, Address::times_1, -entry_displacement + recu_displacement));
  __ increment(t, oopSize);
  __ jmpb(loop);

  __ bind(found);
  // Found the lock
  __ decrement(Address(r15_thread, t, Address::times_1, -entry_displacement + recu_displacement));
  // Decremented to -1, not recursive, fix lock_stack and try unlock
  __ jccb(Assembler::negative, fix_lock_stack);
  // Decremented to x > 0, _has_recu can remain unchanged
  // Set ZF == 1 and jump, // SUCCESS
  __ jcc(Assembler::notZero, set_zf_one);
  // Decremented to 0, must fix the _has_recu field
  // Fallthrough to loop_found

  // Skipped increment, t already points to the next entry
  // no need for ZF juggling, and we know that if we got here
  // the _recu entry for the obj is 0
  __ bind(loop_found);
  __ cmpl(t, Address(r15_thread, JavaThread::lock_stack_top_offset()));
  __ jcc(Assembler::equal, fix_has_recu);
  // Check for other recursion
  __ orq(has_recu, Address(r15_thread, t, Address::times_1, recu_displacement));
  __ increment(t, oopSize);
  __ jmpb(loop_found);

  __ bind(fix_has_recu);
  // We succeeded here but may need to set _has_recu = false
  // if fix_has_recu != 0, then set ZF == 1 and jmp // Success
  __ testq(has_recu, has_recu);
  __ jcc(Assembler::notZero, set_zf_one);
  __ movbool(Address(r15_thread, JavaThread::lock_stack_has_recu_offset()), false);
  __ jmp(zf_one);

  __ bind(fix_lock_stack);
  // The current lock was not recursive, try to lock
  // we forget about has_recu here, because some other lock must
  // be a recursive lock, we do not have to update the _has_recu value
  // Do not have to fix the -1 value in our recur entry, it will be
  // restored when we shift down (and clear the last) recur entries.
  Register t2 = has_recu;

  // First shift down the lock stack, this removes the current oop and recur
  // t == the oops base entry - entry_displacement, when we get here

  Label shift_loop, shift_done;
  __ bind(shift_loop);
  __ cmpl(t, Address(r15_thread, JavaThread::lock_stack_top_offset()));
  __ jcc(Assembler::equal, shift_done);
  // _base[i] = _base[i+1];
  __ movq(t2, Address(r15_thread, t));
  __ movq(Address(r15_thread, t, Address::times_1, -entry_displacement), t2);
  // _recu[i] = _recu[i+1];
  __ movq(t2, Address(r15_thread, t, Address::times_1, recu_displacement));
  __ movq(Address(r15_thread, t, Address::times_1, -entry_displacement + recu_displacement), t2);
  __ increment(t, oopSize);
  __ jmpb(shift_loop);

  __ bind(shift_done);
  // push the obj onto the lock stack in case the cas fails,
  // do it here because we don't want to reload the _top address
  // and we can branch directly to the slow_path
  // _base[to_index(_top) - 1] = obj;
  __ movq(Address(r15_thread, t, Address::times_1, -entry_displacement), obj);
  // _recu[to_index(_top) - 1] = 0;
  __ movq(Address(r15_thread, t, Address::times_1, -entry_displacement + recu_displacement), 0);


  // try to unlock the object now with a cas,
  assert(t2 == rax, "must be");
  // Load the header
  __ movptr(t2, Address(obj, oopDesc::mark_offset_in_bytes()));
  // Make sure we are testing against a fast_locked header
  __ andptr(t2, ~(int32_t)markWord::lock_mask_in_place);
  // Create unlocked header
  __ movptr(t, t2);
  __ orptr(t, markWord::unlocked_value);
  // Try to unlock, cas header
  __ lock();
  __ cmpxchgptr(t, Address(obj, oopDesc::mark_offset_in_bytes()));
  // jump out if cas failed, so ZF == 0 here // FAIL
  __ jcc(Assembler::notEqual, zf_zero);
  // We are now unlocked
  // Pop the lock object from the lock-stack.
  __ decrementl(Address(r15_thread, JavaThread::lock_stack_top_offset()), oopSize);
#ifdef ASSERT
  __ movl(t, Address(r15_thread, JavaThread::lock_stack_top_offset()));
  __ movptr(Address(r15_thread, t), 0);
#endif
  __ bind(set_zf_one);
  __ xorq(t, t);
  __ jmp(zf_one);
}
#endif

#undef __
