/*
 * Copyright (c) 2003, 2013, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2007, 2008, 2009, 2010 Red Hat, Inc.
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

// no precompiled headers
#include "assembler_ppc.inline.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/icBuffer.hpp"
#include "code/vtableStubs.hpp"
#include "interpreter/interpreter.hpp"
#include "jvm_bsd.h"
#include "memory/allocation.inline.hpp"
#include "mutex_bsd.inline.hpp"
#include "nativeInst_ppc.hpp"
#include "os_share_bsd.hpp"
#include "prims/jniFastGetField.hpp"
#include "prims/jvm.h"
#include "prims/jvm_misc.hpp"
#include "runtime/arguments.hpp"
#include "runtime/extendedPC.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/interfaceSupport.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/osThread.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/timer.hpp"
#include "utilities/events.hpp"
#include "utilities/vmError.hpp"
#ifdef COMPILER1
#include "c1/c1_Runtime1.hpp"
#endif
#ifdef COMPILER2
#include "opto/runtime.hpp"
#endif

// put OS-includes here
# include <sys/types.h>
# include <sys/mman.h>
# include <pthread.h>
# include <signal.h>
# include <errno.h>
# include <dlfcn.h>
# include <stdlib.h>
# include <stdio.h>
# include <unistd.h>
# include <sys/resource.h>
# include <pthread_np.h>
# include <sys/stat.h>
# include <sys/time.h>
# include <sys/utsname.h>
# include <sys/socket.h>
# include <sys/wait.h>
# include <pwd.h>
# include <poll.h>
# include <ucontext.h>
#ifdef __FreeBSD__
# include <sys/sysctl.h>
# include <sys/procctl.h>
# ifndef PROC_STACKGAP_STATUS
#  define PROC_STACKGAP_STATUS	18
# endif
# ifndef PROC_STACKGAP_DISABLE
#  define PROC_STACKGAP_DISABLE	0x0002
# endif
#endif /* __FreeBSD__ */


address os::current_stack_pointer() {
  intptr_t* csp;

  // inline assembly `mr regno(csp), R1_SP':
  __asm__ __volatile__ ("mr %0, 1":"=r"(csp):);

  return (address) csp;
}

char* os::non_memory_address_word() {
  // Must never look like an address returned by reserve_memory,
  // even in its subfields (as defined by the CPU immediate fields,
  // if the CPU splits constants across multiple instructions).

  return (char*) -1;
}

void os::initialize_thread(Thread *thread) { }

address os::Bsd::ucontext_get_pc(ucontext_t * uc) {
  guarantee(uc->uc_mcontext.mc_gpr != NULL, "only use ucontext_get_pc in sigaction context");
  return (address)uc->uc_mcontext.mc_srr0;
}

intptr_t* os::Bsd::ucontext_get_sp(ucontext_t * uc) {
  return (intptr_t*)uc->uc_mcontext.mc_gpr[1/*REG_SP*/];
}

intptr_t* os::Bsd::ucontext_get_fp(ucontext_t * uc) {
  return NULL;
}

ExtendedPC os::fetch_frame_from_context(void* ucVoid,
                    intptr_t** ret_sp, intptr_t** ret_fp) {

  ExtendedPC  epc;
  ucontext_t* uc = (ucontext_t*)ucVoid;

  if (uc != NULL) {
    epc = ExtendedPC(os::Bsd::ucontext_get_pc(uc));
    if (ret_sp) *ret_sp = os::Bsd::ucontext_get_sp(uc);
    if (ret_fp) *ret_fp = os::Bsd::ucontext_get_fp(uc);
  } else {
    // construct empty ExtendedPC for return value checking
    epc = ExtendedPC(NULL);
    if (ret_sp) *ret_sp = (intptr_t *)NULL;
    if (ret_fp) *ret_fp = (intptr_t *)NULL;
  }

  return epc;
}

frame os::fetch_frame_from_context(void* ucVoid) {
  intptr_t* sp;
  intptr_t* fp;
  ExtendedPC epc = fetch_frame_from_context(ucVoid, &sp, &fp);
  return frame(sp, epc.pc());
}

frame os::get_sender_for_C_frame(frame* fr) {
  if (*fr->sp() == 0) {
    // fr is the last C frame
    return frame(NULL, NULL);
  }
  return frame(fr->sender_sp(), fr->sender_pc());
}


frame os::current_frame() {
  intptr_t* csp = (intptr_t*) *((intptr_t*) os::current_stack_pointer());
  // hack.
  frame topframe(csp, (address)0x8);
  // return sender of current topframe which hopefully has pc != NULL.
  return os::get_sender_for_C_frame(&topframe);
}

// Utility functions

extern "C" JNIEXPORT int
JVM_handle_bsd_signal(int sig, siginfo_t* info, void* ucVoid, int abort_if_unrecognized) {

  ucontext_t* uc = (ucontext_t*) ucVoid;

  Thread* t = ThreadLocalStorage::get_thread_slow();   // slow & steady

  // Must do this before SignalHandlerMark, if crash protection installed we will longjmp away
  // (no destructors can be run)
  os::ThreadCrashProtection::check_crash_protection(sig, t);

  SignalHandlerMark shm(t);

  // Note: it's not uncommon that JNI code uses signal/sigset to install
  // then restore certain signal handler (e.g. to temporarily block SIGPIPE,
  // or have a SIGILL handler when detecting CPU type). When that happens,
  // JVM_handle_bsd_signal() might be invoked with junk info/ucVoid. To
  // avoid unnecessary crash when libjsig is not preloaded, try handle signals
  // that do not require siginfo/ucontext first.

  if (sig == SIGPIPE || sig == SIGXFSZ) {
    if (os::Bsd::chained_handler(sig, info, ucVoid)) {
      return 1;
    } else {
      if (PrintMiscellaneous && (WizardMode || Verbose)) {
        warning("Ignoring SIGPIPE - see bug 4229104");
      }
      return 1;
    }
  }

  JavaThread* thread = NULL;
  VMThread* vmthread = NULL;
  if (os::Bsd::signal_handlers_are_installed) {
    if (t != NULL) {
      if(t->is_Java_thread()) {
        thread = (JavaThread*)t;
      }
      else if(t->is_VM_thread()) {
        vmthread = (VMThread *)t;
      }
    }
  }

  // Decide if this trap can be handled by a stub.
  address stub = NULL;

  // retrieve program counter
  address const pc = uc ? os::Bsd::ucontext_get_pc(uc) : NULL;

  // retrieve crash address
  address addr = info ? (address) info->si_addr : NULL;

  // Moved SafeFetch32 handling outside thread!=NULL conditional block to make
  // it work if no associated JavaThread object exists.
  if ((sig == SIGSEGV || sig == SIGBUS) && uc) {
    address const pc = os::Bsd::ucontext_get_pc(uc);
    if (pc && StubRoutines::is_safefetch_fault(pc)) {
      uc->uc_mcontext.mc_srr0 = (unsigned long)StubRoutines::continuation_for_safefetch_fault(pc);
      return true;
    }
  }

  // Handle SIGDANGER right away. AIX would raise SIGDANGER whenever available swap
  // space falls below 30%. This is only a chance for the process to gracefully abort.
  // We can't hope to proceed after SIGDANGER since SIGKILL tailgates.
  // if (sig == SIGDANGER) {
  //  goto report_and_die;
  // }

  if (info == NULL || uc == NULL || thread == NULL && vmthread == NULL) {
    goto run_chained_handler;
  }

  // If we are a java thread...
  if (thread != NULL) {
#ifdef __FreeBSD__
    /*
     * Determine whether the kernel stack guard pages have been disabled
     */
    int status = 0;
    int ret = procctl(P_PID, getpid(), PROC_STACKGAP_STATUS, &status);

    /*
     * Check if the call to procctl(2) failed or the stack guard is not
     * disabled.  Either way, we'll then attempt a workaround.
     */
    if (ret == -1 || !(status & PROC_STACKGAP_DISABLE)) {
      /*
       * Try to work around the problems caused on FreeBSD where the kernel
       * may place guard pages above JVM guard pages and prevent the Java
       * thread stacks growing into the JVM guard pages.  The work around
       * is to determine how many such pages there may be and round down the
       * fault address so that tests of whether it is in the JVM guard zone
       * succeed.
       *
       * Note that this is a partial workaround at best since the normally
       * the JVM could then unprotect the reserved area to allow a critical
       * section to complete.  This is not possible if the kernel has
       * placed guard pages below the reserved area.
       *
       * This also suffers from the problem that the
       * security.bsd.stack_guard_page sysctl is dynamic and may have
       * changed since the stack was allocated.  This is likely to be rare
       * in practice though.
       *
       * What this does do is prevent the JVM crashing on FreeBSD and
       * instead throwing a StackOverflowError when infinite recursion
       * is attempted, which is the expected behaviour.  Due to it's
       * limitations though, objects may be in unexpected states when
       * this occurs.
       *
       * A better way to avoid these problems is either to be on a new
       * enough version of FreeBSD (one that has PROC_STACKGAP_CTL) or set
       * security.bsd.stack_guard_page to zero.
       */
      int guard_pages = 0;
      size_t size = sizeof(guard_pages);
      if (sysctlbyname("security.bsd.stack_guard_page",
                       &guard_pages, &size, NULL, 0) == 0 &&
          guard_pages > 0) {
        addr -= guard_pages * os::vm_page_size();
      }
    }
#endif

    // Handle ALL stack overflow variations here
    if (sig == SIGSEGV && (addr < thread->stack_base() &&
                           addr >= thread->stack_base() - thread->stack_size())) {
      // stack overflow
      //
      // If we are in a yellow zone and we are inside java, we disable the yellow zone and
      // throw a stack overflow exception.
      // If we are in native code or VM C code, we report-and-die. The original coding tried
      // to continue with yellow zone disabled, but that doesn't buy us much and prevents
      // hs_err_pid files.
      if (thread->in_stack_yellow_zone(addr)) {
        thread->disable_stack_yellow_zone();
        if (thread->thread_state() == _thread_in_Java) {
          // Throw a stack overflow exception.
          // Guard pages will be reenabled while unwinding the stack.
          stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::STACK_OVERFLOW);
          goto run_stub;
        } else {
          // Thread was in the vm or native code. Return and try to finish.
          return 1;
        }
      } else if (thread->in_stack_red_zone(addr)) {
        // Fatal red zone violation. Disable the guard pages and fall through
        // to handle_unexpected_exception way down below.
        thread->disable_stack_red_zone();
        tty->print_raw_cr("An irrecoverable stack overflow has occurred.");
        goto report_and_die;
      } else {
        // This means a segv happened inside our stack, but not in
        // the guarded zone. I'd like to know when this happens,
        tty->print_raw_cr("SIGSEGV happened inside stack but outside yellow and red zone.");
        goto report_and_die;
      }

    } // end handle SIGSEGV inside stack boundaries

    if (thread->thread_state() == _thread_in_Java) {
      // Java thread running in Java code

      // The following signals are used for communicating VM events:
      //
      // SIGILL: the compiler generates illegal opcodes
      //   at places where it wishes to interrupt the VM:
      //   Safepoints, Unreachable Code, Entry points of Zombie methods,
      //    This results in a SIGILL with (*pc) == inserted illegal instruction.
      //
      //   (so, SIGILLs with a pc inside the zero page are real errors)
      //
      // SIGTRAP:
      //   The ppc trap instruction raises a SIGTRAP and is very efficient if it
      //   does not trap. It is used for conditional branches that are expected
      //   to be never taken. These are:
      //     - zombie methods
      //     - IC (inline cache) misses.
      //     - null checks leading to UncommonTraps.
      //     - range checks leading to Uncommon Traps.
      //   On Bsd, these are especially null checks, as the ImplicitNullCheck
      //   optimization works only in rare cases, as the page at address 0 is only
      //   write protected.      //
      //   Note: !UseSIGTRAP is used to prevent SIGTRAPS altogether, to facilitate debugging.
      //
      // SIGSEGV:
      //   used for safe point polling:
      //     To notify all threads that they have to reach a safe point, safe point polling is used:
      //     All threads poll a certain mapped memory page. Normally, this page has read access.
      //     If the VM wants to inform the threads about impending safe points, it puts this
      //     page to read only ("poisens" the page), and the threads then reach a safe point.
      //   used for null checks:
      //     If the compiler finds a store it uses it for a null check. Unfortunately this
      //     happens rarely.  In heap based and disjoint base compressd oop modes also loads
      //     are used for null checks.

      // A VM-related SIGILL may only occur if we are not in the zero page.
      // On AIX, we get a SIGILL if we jump to 0x0 or to somewhere else
      // in the zero page, because it is filled with 0x0. We ignore
      // explicit SIGILLs in the zero page.
      if (sig == SIGILL && (pc < (address) 0x200)) {
        if (TraceTraps) {
          tty->print_raw_cr("SIGILL happened inside zero page.");
        }
        goto report_and_die;
      }

      // Handle signal from NativeJump::patch_verified_entry().
      if (( TrapBasedNotEntrantChecks && sig == SIGTRAP && nativeInstruction_at(pc)->is_sigtrap_zombie_not_entrant()) ||
          (!TrapBasedNotEntrantChecks && sig == SIGILL  && nativeInstruction_at(pc)->is_sigill_zombie_not_entrant())) {
        if (TraceTraps) {
          tty->print_cr("trap: zombie_not_entrant (%s)", (sig == SIGTRAP) ? "SIGTRAP" : "SIGILL");
        }
        stub = SharedRuntime::get_handle_wrong_method_stub();
        goto run_stub;
      }

      else if (sig == SIGSEGV && os::is_poll_address(addr)) {
        if (TraceTraps) {
          tty->print_cr("trap: safepoint_poll at " INTPTR_FORMAT " (SIGSEGV)", pc);
        }
        stub = SharedRuntime::get_poll_stub(pc);
        goto run_stub;
      }

      // SIGTRAP-based ic miss check in compiled code.
      else if (sig == SIGTRAP && TrapBasedICMissChecks &&
               nativeInstruction_at(pc)->is_sigtrap_ic_miss_check()) {
        if (TraceTraps) {
          tty->print_cr("trap: ic_miss_check at " INTPTR_FORMAT " (SIGTRAP)", pc);
        }
        stub = SharedRuntime::get_ic_miss_stub();
        goto run_stub;
      }

      // SIGTRAP-based implicit null check in compiled code.
      else if (sig == SIGTRAP && TrapBasedNullChecks &&
               nativeInstruction_at(pc)->is_sigtrap_null_check()) {
        if (TraceTraps) {
          tty->print_cr("trap: null_check at " INTPTR_FORMAT " (SIGTRAP)", pc);
        }
        stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::IMPLICIT_NULL);
        goto run_stub;
      }

      // SIGSEGV-based implicit null check in compiled code.
      else if (sig == SIGSEGV && ImplicitNullChecks &&
               CodeCache::contains((void*) pc) &&
               !MacroAssembler::needs_explicit_null_check((intptr_t) info->si_addr)) {
        if (TraceTraps) {
          tty->print_cr("trap: null_check at " INTPTR_FORMAT " (SIGSEGV)", pc);
        }
        stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::IMPLICIT_NULL);
      }

#ifdef COMPILER2
      // SIGTRAP-based implicit range check in compiled code.
      else if (sig == SIGTRAP && TrapBasedRangeChecks &&
               nativeInstruction_at(pc)->is_sigtrap_range_check()) {
        if (TraceTraps) {
          tty->print_cr("trap: range_check at " INTPTR_FORMAT " (SIGTRAP)", pc);
        }
        stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::IMPLICIT_NULL);
        goto run_stub;
      }
#endif

      else if (sig == SIGFPE /* && info->si_code == FPE_INTDIV */) {
        if (TraceTraps) {
          tty->print_raw_cr("Fix SIGFPE handler, trying divide by zero handler.");
        }
        stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::IMPLICIT_DIVIDE_BY_ZERO);
        goto run_stub;
      }

      else if (sig == SIGBUS) {
        // BugId 4454115: A read from a MappedByteBuffer can fault here if the
        // underlying file has been truncated. Do not crash the VM in such a case.
        CodeBlob* cb = CodeCache::find_blob_unsafe(pc);
        nmethod* nm = cb->is_nmethod() ? (nmethod*)cb : NULL;
        if (nm != NULL && nm->has_unsafe_access()) {
          // We don't really need a stub here! Just set the pending exeption and
          // continue at the next instruction after the faulting read. Returning
          // garbage from this read is ok.
          thread->set_pending_unsafe_access_error();
          uc->uc_mcontext.mc_srr0 = ((unsigned long)pc) + 4;
          return 1;
        }
      }
    }

    else { // thread->thread_state() != _thread_in_Java
      // Detect CPU features. This is only done at the very start of the VM. Later, the
      // VM_Version::is_determine_features_test_running() flag should be false.

      if (sig == SIGILL && VM_Version::is_determine_features_test_running()) {
        // SIGILL must be caused by VM_Version::determine_features().
        *(int *)pc = 0; // patch instruction to 0 to indicate that it causes a SIGILL,
                        // flushing of icache is not necessary.
        stub = pc + 4;  // continue with next instruction.
        goto run_stub;
      }
      else if (thread->thread_state() == _thread_in_vm &&
               sig == SIGBUS && thread->doing_unsafe_access()) {
        // We don't really need a stub here! Just set the pending exeption and
        // continue at the next instruction after the faulting read. Returning
        // garbage from this read is ok.
        thread->set_pending_unsafe_access_error();
        uc->uc_mcontext.mc_srr0 = ((unsigned long)pc) + 4;
        return 1;
      }
    }

    // Check to see if we caught the safepoint code in the
    // process of write protecting the memory serialization page.
    // It write enables the page immediately after protecting it
    // so we can just return to retry the write.
    if ((sig == SIGSEGV) &&
        os::is_memory_serialize_page(thread, addr)) {
      // Synchronization problem in the pseudo memory barrier code (bug id 6546278)
      // Block current thread until the memory serialize page permission restored.
      os::block_on_serialize_page_trap();
      return true;
    }
  }

run_stub:

  // One of the above code blocks ininitalized the stub, so we want to
  // delegate control to that stub.
  if (stub != NULL) {
    // Save all thread context in case we need to restore it.
    if (thread != NULL) thread->set_saved_exception_pc(pc);
    uc->uc_mcontext.mc_srr0 = (unsigned long)stub;
    return 1;
  }

run_chained_handler:

  // signal-chaining
  if (os::Bsd::chained_handler(sig, info, ucVoid)) {
    return 1;
  }
  if (!abort_if_unrecognized) {
    // caller wants another chance, so give it to him
    return 0;
  }

report_and_die:

  // Use sigthreadmask instead of sigprocmask on AIX and unmask current signal.
  sigset_t newset;
  sigemptyset(&newset);
  sigaddset(&newset, sig);
  sigprocmask(SIG_UNBLOCK, &newset, NULL);

  VMError err(t, sig, pc, info, ucVoid);
  err.report_and_die();

  ShouldNotReachHere();
  return 0;
}

void os::Bsd::init_thread_fpu_state(void) {
  // Disable FP exceptions.
  __asm__ __volatile__ ("mtfsfi 6,0");
}

///////////////////////////////////////////////////////////////////////////////
// thread stack

size_t os::Bsd::min_stack_allowed = 128*K;

bool os::Bsd::supports_variable_stack_size() { return true; }

// return default stack size for thr_type
size_t os::Bsd::default_stack_size(os::ThreadType thr_type) {
  // default stack size (compiler thread needs larger stack)
  // Notice that the setting for compiler threads here have no impact
  // because of the strange 'fallback logic' in os::create_thread().
  // Better set CompilerThreadStackSize in globals_<os_cpu>.hpp if you want to
  // specify a different stack size for compiler threads!
  size_t s = (thr_type == os::compiler_thread ? 4 * M : 1024 * K);
  return s;
}

size_t os::Bsd::default_guard_size(os::ThreadType thr_type) {
  return 2 * page_size();
}

// Java thread:
//
//   Low memory addresses
//    +------------------------+
//    |                        |\  JavaThread created by VM does not have glibc
//    |    glibc guard page    | - guard, attached Java thread usually has
//    |                        |/  1 page glibc guard.
// P1 +------------------------+ Thread::stack_base() - Thread::stack_size()
//    |                        |\
//    |  HotSpot Guard Pages   | - red and yellow pages
//    |                        |/
//    +------------------------+ JavaThread::stack_yellow_zone_base()
//    |                        |\
//    |      Normal Stack      | -
//    |                        |/
// P2 +------------------------+ Thread::stack_base()
//
// Non-Java thread:
//
//   Low memory addresses
//    +------------------------+
//    |                        |\
//    |  glibc guard page      | - usually 1 page
//    |                        |/
// P1 +------------------------+ Thread::stack_base() - Thread::stack_size()
//    |                        |\
//    |      Normal Stack      | -
//    |                        |/
// P2 +------------------------+ Thread::stack_base()
//
// ** P1 (aka bottom) and size ( P2 = P1 - size) are the address and stack size returned from
//    pthread_attr_getstack()

static void current_stack_region(address * bottom, size_t * size) {
#ifdef __APPLE__
  pthread_t self = pthread_self();
  void *stacktop = pthread_get_stackaddr_np(self);
  *size = pthread_get_stacksize_np(self);
  // workaround for OS X 10.9.0 (Mavericks)
  // pthread_get_stacksize_np returns 128 pages even though the actual size is 2048 pages
  if (pthread_main_np() == 1) {
    if ((*size) < (DEFAULT_MAIN_THREAD_STACK_PAGES * (size_t)getpagesize())) {
      char kern_osrelease[256];
      size_t kern_osrelease_size = sizeof(kern_osrelease);
      int ret = sysctlbyname("kern.osrelease", kern_osrelease, &kern_osrelease_size, NULL, 0);
      if (ret == 0) {
        // get the major number, atoi will ignore the minor amd micro portions of the version string
        if (atoi(kern_osrelease) >= OS_X_10_9_0_KERNEL_MAJOR_VERSION) {
          *size = (DEFAULT_MAIN_THREAD_STACK_PAGES*getpagesize());
        }
      }
    }
  }
  *bottom = (address) stacktop - *size;
#elif defined(__OpenBSD__)
  stack_t ss;
  int rslt = pthread_stackseg_np(pthread_self(), &ss);

  if (rslt != 0)
    fatal(err_msg("pthread_stackseg_np failed with err = %d", rslt));

  *bottom = (address)((char *)ss.ss_sp - ss.ss_size);
  *size   = ss.ss_size;
#else
  pthread_attr_t attr;

  int rslt = pthread_attr_init(&attr);

  // JVM needs to know exact stack location, abort if it fails
  if (rslt != 0)
    fatal(err_msg("pthread_attr_init failed with err = %d", rslt));

  rslt = pthread_attr_get_np(pthread_self(), &attr);

  if (rslt != 0)
    fatal(err_msg("pthread_attr_get_np failed with err = %d", rslt));

  if (pthread_attr_getstackaddr(&attr, (void **)bottom) != 0 ||
    pthread_attr_getstacksize(&attr, size) != 0) {
    fatal("Can not locate current stack attributes!");
  }

  pthread_attr_destroy(&attr);
#endif
  assert(os::current_stack_pointer() >= *bottom &&
         os::current_stack_pointer() < *bottom + *size, "just checking");
}

address os::current_stack_base() {
  address bottom;
  size_t size;
  current_stack_region(&bottom, &size);
  return (bottom + size);
}

size_t os::current_stack_size() {
  // stack size includes normal stack and HotSpot guard pages
  address bottom;
  size_t size;
  current_stack_region(&bottom, &size);
  return size;
}

/////////////////////////////////////////////////////////////////////////////
// helper functions for fatal error handler

void os::print_context(outputStream *st, void *context) {
  if (context == NULL) return;

  ucontext_t* uc = (ucontext_t*)context;

  st->print_cr("Registers:");
  st->print("pc =" INTPTR_FORMAT "  ", uc->uc_mcontext.mc_srr0);
  st->print("lr =" INTPTR_FORMAT "  ", uc->uc_mcontext.mc_lr);
  st->print("ctr=" INTPTR_FORMAT "  ", uc->uc_mcontext.mc_ctr);
  st->cr();
  for (int i = 0; i < 32; i++) {
    st->print("r%-2d=" INTPTR_FORMAT "  ", i, uc->uc_mcontext.mc_gpr[i]);
    if (i % 3 == 2) st->cr();
  }
  st->cr();
  st->cr();

  intptr_t *sp = (intptr_t *)os::Bsd::ucontext_get_sp(uc);
  st->print_cr("Top of Stack: (sp=" PTR_FORMAT ")", p2i(sp));
  print_hex_dump(st, (address)sp, (address)(sp + 128), sizeof(intptr_t));
  st->cr();

  // Note: it may be unsafe to inspect memory near pc. For example, pc may
  // point to garbage if entry point in an nmethod is corrupted. Leave
  // this at the end, and hope for the best.
  address pc = os::Bsd::ucontext_get_pc(uc);
  st->print_cr("Instructions: (pc=" PTR_FORMAT ")", p2i(pc));
  print_hex_dump(st, pc - 64, pc + 64, /*instrsize=*/4);
  st->cr();
}

void os::print_register_info(outputStream *st, void *context) {
  if (context == NULL) return;

  ucontext_t *uc = (ucontext_t*)context;

  st->print_cr("Register to memory mapping:");
  st->cr();

  // this is only for the "general purpose" registers
  for (int i = 0; i < 32; i++) {
    st->print("r%-2d=", i);
    print_location(st, uc->uc_mcontext.mc_gpr[i]);
  }
  st->cr();
}

extern "C" {
  int SpinPause() {
    return 0;
  }
}

#ifndef PRODUCT
void os::verify_stack_alignment() {
  assert(((intptr_t)os::current_stack_pointer() & (StackAlignmentInBytes-1)) == 0, "incorrect stack alignment");
}
#endif
