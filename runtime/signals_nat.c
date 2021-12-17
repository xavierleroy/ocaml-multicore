/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*             Xavier Leroy, projet Gallium, INRIA Rocquencourt           */
/*                                                                        */
/*   Copyright 2007 Institut National de Recherche en Informatique et     */
/*     en Automatique.                                                    */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#define CAML_INTERNALS

/* Signal handling, code specific to the native-code compiler */

#if defined(TARGET_amd64) && defined (SYS_linux)
#define _GNU_SOURCE
#endif
#if defined(TARGET_i386) && defined (SYS_linux_elf)
#define _GNU_SOURCE
#endif
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include "caml/codefrag.h"
#include "caml/domain.h"
#include "caml/fail.h"
#include "caml/fiber.h"
#include "caml/frame_descriptors.h"
#include "caml/memory.h"
#include "caml/osdeps.h"
#include "caml/signals.h"
#include "caml/stack.h"

#ifndef NSIG
#define NSIG 64
#endif

typedef void (*signal_handler)(int signo);

#ifdef _WIN32
extern signal_handler caml_win32_signal(int sig, signal_handler action);
#define signal(sig,act) caml_win32_signal(sig,act)
#endif

/* This routine is the common entry point for garbage collection
   and signal handling.  It can trigger a callback to OCaml code.
   With system threads, this callback can cause a context switch.
   Hence [caml_garbage_collection] must not be called from regular C code
   (e.g. the [caml_alloc] function) because the context of the call
   (e.g. [intern_val]) may not allow context switching.
   Only generated assembly code can call [caml_garbage_collection],
   via the caml_call_gc assembly stubs.  */

void caml_garbage_collection()
{
  frame_descr* d;
  intnat allocsz = 0;
  char *sp;
  uintnat retaddr;
  intnat whsize;

  caml_frame_descrs fds = caml_get_frame_descrs();
  struct stack_info* stack = Caml_state->current_stack;

  sp = (char*)stack->sp;
  retaddr = *(uintnat*)sp;

  { /* Find the frame descriptor for the current allocation */
    uintnat h = Hash_retaddr(retaddr, fds.mask);
    while (1) {
      d = fds.descriptors[h];
      if (d->retaddr == retaddr) break;
      h = (h + 1) & fds.mask;
    }
    /* Must be an allocation frame */
    CAMLassert(d && d->frame_size != 0xFFFF && (d->frame_size & 2));
  }

  { /* Compute the total allocation size at this point,
       including allocations combined by Comballoc */
    unsigned char* alloc_len = (unsigned char*)(&d->live_ofs[d->num_live]);
    int i, nallocs = *alloc_len++;

    if (nallocs == 0) {
      /* This is a poll */
      caml_handle_gc_interrupt(); // process pending actions?
      caml_process_pending_signals();
      return;
    }
    else
    {
      for (i = 0; i < nallocs; i++) {
        allocsz += Whsize_wosize(Wosize_encoded_alloc_len(alloc_len[i]));
      }
      /* We have computed whsize (including header)
         but need wosize (without) */
      allocsz -= 1;
    }

    whsize = Whsize_wosize(allocsz);

    /* Put the young pointer back to what is was before our tiggering
       allocation */
    Caml_state->young_ptr += whsize;

    /* When caml_garbage_collection returns, we assume there is enough space in
      the minor heap for the triggering allocation. Due to finalisers in the
      major heap, it is possible for there to be a sequence of events where a
      single call to caml_handle_gc_interrupt does not lead to that. We do it
      in a loop to ensure it. */
    do {
      caml_handle_gc_interrupt();
      caml_process_pending_signals();
    } while
       ( (uintnat)(Caml_state->young_ptr - whsize) <= Caml_state->young_limit );

    /* Re-do the allocation: we now have enough space in the minor heap. */
    Caml_state->young_ptr -= whsize;
  }
}

static void handle_signal(int sig)
{
  int saved_errno;
  /* Save the value of errno (PR#5982). */
  saved_errno = errno;
#if !defined(POSIX_SIGNALS) && !defined(BSD_SIGNALS)
  signal(sig, handle_signal);
#endif
  if (sig < 0 || sig >= NSIG) return;
  caml_record_signal(sig);
  errno = saved_errno;
}

int caml_set_signal_action(int signo, int action)
{
  signal_handler act, oldact;
#ifdef POSIX_SIGNALS
  struct sigaction sigact, oldsigact;
#endif

  switch(action) {
  case 0:  act = SIG_DFL; break;
  case 1:  act = SIG_IGN; break;
  default: act = handle_signal; break;
  }

#ifdef POSIX_SIGNALS
  sigact.sa_handler = act;
  sigemptyset(&sigact.sa_mask);
  sigact.sa_flags = 0;
  if (sigaction(signo, &sigact, &oldsigact) == -1) return -1;
  oldact = oldsigact.sa_handler;
#else
  oldact = signal(signo, act);
  if (oldact == SIG_ERR) return -1;
#endif
  if (oldact == handle_signal)
    return 2;
  else if (oldact == SIG_IGN)
    return 1;
  else
    return 0;
}

/* Machine- and OS-dependent handling of bound check trap */

#if defined(TARGET_power) \
  || defined(TARGET_s390x)
#error "Architecture requires a bounds-check trap handler"
static void trap_handler(int sig, siginfo_t * info, void * context)
{
  /* TODO: raise a real exception here */
  caml_fatal_error ("bounds check failed");
}

#endif

/* Initialization of signal stuff */

void caml_init_signals(void)
{
  /* Bound-check trap handling */

#if defined(TARGET_power)
  { struct sigaction act;
    act.sa_sigaction = trap_handler;
    act.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&act.sa_mask);
    sigaction(SIGTRAP, &act, NULL);
  }
#endif

#if defined(TARGET_s390x)
  { struct sigaction act;
    act.sa_sigaction = trap_handler;
    act.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&act.sa_mask);
    sigaction(SIGFPE, &act, NULL);
  }
#endif
}
