/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include "lint.h"
#include "thr_uberdata.h"
#include "asyncio.h"
#include <signal.h>
#include <siginfo.h>
#include <ucontext.h>
#include <sys/systm.h>

const sigset_t maskset = {MASKSET0, MASKSET1, 0, 0};	/* maskable signals */

/*
 * Return true if the valid signal bits in both sets are the same.
 */
int
sigequalset(const sigset_t *s1, const sigset_t *s2)
{
	/*
	 * We only test valid signal bits, not rubbish following MAXSIG
	 * (for speed).  Algorithm:
	 * if (s1 & fillset) == (s2 & fillset) then (s1 ^ s2) & fillset == 0
	 */
	return (!((s1->__sigbits[0] ^ s2->__sigbits[0]) |
	    ((s1->__sigbits[1] ^ s2->__sigbits[1]) & FILLSET1)));
}

/*
 * Common code for calling the user-specified signal handler.
 */
void
call_user_handler(int sig, siginfo_t *sip, ucontext_t *ucp)
{
	ulwp_t *self = curthread;
	uberdata_t *udp = self->ul_uberdata;
	struct sigaction uact;
	volatile struct sigaction *sap;

	/*
	 * If we are taking a signal while parked or about to be parked
	 * on __lwp_park() then remove ourself from the sleep queue so
	 * that we can grab locks.  The code in mutex_lock_queue() and
	 * cond_wait_common() will detect this and deal with it when
	 * __lwp_park() returns.
	 */
	unsleep_self();
	set_parking_flag(self, 0);

	if (__td_event_report(self, TD_CATCHSIG, udp)) {
		self->ul_td_evbuf.eventnum = TD_CATCHSIG;
		self->ul_td_evbuf.eventdata = (void *)(intptr_t)sig;
		tdb_event(TD_CATCHSIG, udp);
	}

	/*
	 * Get a self-consistent set of flags, handler, and mask
	 * while holding the sig's sig_lock for the least possible time.
	 * We must acquire the sig's sig_lock because some thread running
	 * in sigaction() might be establishing a new signal handler.
	 *
	 * Locking exceptions:
	 * No locking for a child of vfork().
	 * If the signal is SIGPROF with an si_code of PROF_SIG,
	 * then we assume that this signal was generated by
	 * setitimer(ITIMER_REALPROF) set up by the dbx collector.
	 * If the signal is SIGEMT with an si_code of EMT_CPCOVF,
	 * then we assume that the signal was generated by
	 * a hardware performance counter overflow.
	 * In these cases, assume that we need no locking.  It is the
	 * monitoring program's responsibility to ensure correctness.
	 */
	sap = &udp->siguaction[sig].sig_uaction;
	if (self->ul_vfork ||
	    (sip != NULL &&
	    ((sig == SIGPROF && sip->si_code == PROF_SIG) ||
	    (sig == SIGEMT && sip->si_code == EMT_CPCOVF)))) {
		/* we wish this assignment could be atomic */
		(void) _private_memcpy(&uact, (void *)sap, sizeof (uact));
	} else {
		mutex_t *mp = &udp->siguaction[sig].sig_lock;
		lmutex_lock(mp);
		(void) _private_memcpy(&uact, (void *)sap, sizeof (uact));
		if (sig == SIGCANCEL && (sap->sa_flags & SA_RESETHAND))
			sap->sa_sigaction = SIG_DFL;
		lmutex_unlock(mp);
	}

	/*
	 * Set the proper signal mask and call the user's signal handler.
	 * (We overrode the user-requested signal mask with maskset
	 * so we currently have all blockable signals blocked.)
	 *
	 * We would like to ASSERT() that the signal is not a member of the
	 * signal mask at the previous level (ucp->uc_sigmask) or the specified
	 * signal mask for sigsuspend() or pollsys() (self->ul_tmpmask) but
	 * /proc can override this via PCSSIG, so we don't bother.
	 *
	 * We would also like to ASSERT() that the signal mask at the previous
	 * level equals self->ul_sigmask (maskset for sigsuspend() / pollsys()),
	 * but /proc can change the thread's signal mask via PCSHOLD, so we
	 * don't bother with that either.
	 */
	ASSERT(ucp->uc_flags & UC_SIGMASK);
	if (self->ul_sigsuspend) {
		ucp->uc_sigmask = self->ul_sigmask;
		self->ul_sigsuspend = 0;
		/* the sigsuspend() or pollsys() signal mask */
		sigorset(&uact.sa_mask, &self->ul_tmpmask);
	} else {
		/* the signal mask at the previous level */
		sigorset(&uact.sa_mask, &ucp->uc_sigmask);
	}
	if (!(uact.sa_flags & SA_NODEFER))	/* add current signal */
		(void) _private_sigaddset(&uact.sa_mask, sig);
	self->ul_sigmask = uact.sa_mask;
	self->ul_siglink = ucp;
	(void) __lwp_sigmask(SIG_SETMASK, &uact.sa_mask, NULL);

	/*
	 * If this thread has been sent SIGCANCEL from the kernel
	 * or from pthread_cancel(), it is being asked to exit.
	 * The kernel may send SIGCANCEL without a siginfo struct.
	 * If the SIGCANCEL is process-directed (from kill() or
	 * sigqueue()), treat it as an ordinary signal.
	 */
	if (sig == SIGCANCEL) {
		if (sip == NULL || SI_FROMKERNEL(sip) ||
		    sip->si_code == SI_LWP) {
			do_sigcancel();
			goto out;
		}
		/* SIGCANCEL is ignored by default */
		if (uact.sa_sigaction == SIG_DFL ||
		    uact.sa_sigaction == SIG_IGN)
			goto out;
	}

	/*
	 * If this thread has been sent SIGAIOCANCEL (SIGLWP) and
	 * we are an aio worker thread, cancel the aio request.
	 */
	if (sig == SIGAIOCANCEL) {
		aio_worker_t *aiowp = _pthread_getspecific(_aio_key);

		if (sip != NULL && sip->si_code == SI_LWP && aiowp != NULL)
			_siglongjmp(aiowp->work_jmp_buf, 1);
		/* SIGLWP is ignored by default */
		if (uact.sa_sigaction == SIG_DFL ||
		    uact.sa_sigaction == SIG_IGN)
			goto out;
	}

	if (!(uact.sa_flags & SA_SIGINFO))
		sip = NULL;
	__sighndlr(sig, sip, ucp, uact.sa_sigaction);

#if defined(sparc) || defined(__sparc)
	/*
	 * If this is a floating point exception and the queue
	 * is non-empty, pop the top entry from the queue.  This
	 * is to maintain expected behavior.
	 */
	if (sig == SIGFPE && ucp->uc_mcontext.fpregs.fpu_qcnt) {
		fpregset_t *fp = &ucp->uc_mcontext.fpregs;

		if (--fp->fpu_qcnt > 0) {
			unsigned char i;
			struct fq *fqp;

			fqp = fp->fpu_q;
			for (i = 0; i < fp->fpu_qcnt; i++)
				fqp[i] = fqp[i+1];
		}
	}
#endif	/* sparc */

out:
	(void) _private_setcontext(ucp);
	thr_panic("call_user_handler(): _setcontext() returned");
}

/*
 * take_deferred_signal() is called when ul_critical and ul_sigdefer become
 * zero and a deferred signal has been recorded on the current thread.
 * We are out of the critical region and are ready to take a signal.
 * The kernel has all signals blocked on this lwp, but our value of
 * ul_sigmask is the correct signal mask for the previous context.
 */
void
take_deferred_signal(int sig)
{
	ulwp_t *self = curthread;
	siginfo_t siginfo;
	siginfo_t *sip;
	ucontext_t uc;
	volatile int returning;

	ASSERT(self->ul_critical == 0);
	ASSERT(self->ul_sigdefer == 0);
	ASSERT(self->ul_cursig == 0);

	returning = 0;
	uc.uc_flags = UC_ALL;
	/*
	 * We call _private_getcontext (a libc-private synonym for
	 * _getcontext) rather than _getcontext because we need to
	 * avoid the dynamic linker and link auditing problems here.
	 */
	(void) _private_getcontext(&uc);
	/*
	 * If the application signal handler calls setcontext() on
	 * the ucontext we give it, it returns here, then we return.
	 */
	if (returning)
		return;
	returning = 1;
	ASSERT(sigequalset(&uc.uc_sigmask, &maskset));
	if (self->ul_siginfo.si_signo == 0)
		sip = NULL;
	else {
		(void) _private_memcpy(&siginfo,
		    &self->ul_siginfo, sizeof (siginfo));
		sip = &siginfo;
	}
	uc.uc_sigmask = self->ul_sigmask;
	call_user_handler(sig, sip, &uc);
}

void
sigacthandler(int sig, siginfo_t *sip, void *uvp)
{
	ucontext_t *ucp = uvp;
	ulwp_t *self = curthread;

	/*
	 * Do this in case we took a signal while in a cancelable system call.
	 * It does no harm if we were not in such a system call.
	 */
	self->ul_sp = 0;
	if (sig != SIGCANCEL)
		self->ul_cancel_async = self->ul_save_async;

	/*
	 * If we are not in a critical region and are
	 * not deferring signals, take the signal now.
	 */
	if ((self->ul_critical + self->ul_sigdefer) == 0) {
		call_user_handler(sig, sip, ucp);
		return;	/* call_user_handler() cannot return */
	}

	/*
	 * We are in a critical region or we are deferring signals.  When
	 * we emerge from the region we will call take_deferred_signal().
	 */
	ASSERT(self->ul_cursig == 0);
	self->ul_cursig = (char)sig;
	if (sip != NULL)
		(void) _private_memcpy(&self->ul_siginfo,
		    sip, sizeof (siginfo_t));
	else
		self->ul_siginfo.si_signo = 0;

	/*
	 * Make sure that if we return to a call to __lwp_park()
	 * or ___lwp_cond_wait() that it returns right away
	 * (giving us a spurious wakeup but not a deadlock).
	 */
	set_parking_flag(self, 0);

	/*
	 * Return to the previous context with all signals blocked.
	 * We will restore the signal mask in take_deferred_signal().
	 * Note that we are calling the system call trap here, not
	 * the _setcontext() wrapper.  We don't want to change the
	 * thread's ul_sigmask by this operation.
	 */
	ucp->uc_sigmask = maskset;
	(void) __setcontext_syscall(ucp);
	thr_panic("sigacthandler(): __setcontext() returned");
}

#pragma weak sigaction = _libc_sigaction
#pragma weak _sigaction = _libc_sigaction
int
_libc_sigaction(int sig, const struct sigaction *nact, struct sigaction *oact)
{
	ulwp_t *self = curthread;
	uberdata_t *udp = self->ul_uberdata;
	struct sigaction oaction;
	struct sigaction tact;
	struct sigaction *tactp = NULL;
	int rv;

	if (sig <= 0 || sig >= NSIG) {
		errno = EINVAL;
		return (-1);
	}

	if (!self->ul_vfork)
		lmutex_lock(&udp->siguaction[sig].sig_lock);

	oaction = udp->siguaction[sig].sig_uaction;

	if (nact != NULL) {
		tact = *nact;	/* make a copy so we can modify it */
		tactp = &tact;
		delete_reserved_signals(&tact.sa_mask);

#if !defined(_LP64)
		tact.sa_resv[0] = tact.sa_resv[1] = 0;	/* cleanliness */
#endif
		/*
		 * To be compatible with the behavior of SunOS 4.x:
		 * If the new signal handler is SIG_IGN or SIG_DFL, do
		 * not change the signal's entry in the siguaction array.
		 * This allows a child of vfork(2) to set signal handlers
		 * to SIG_IGN or SIG_DFL without affecting the parent.
		 *
		 * This also covers a race condition with some thread
		 * setting the signal action to SIG_DFL or SIG_IGN
		 * when the thread has also received and deferred
		 * that signal.  When the thread takes the deferred
		 * signal, even though it has set the action to SIG_DFL
		 * or SIG_IGN, it will execute the old signal handler
		 * anyway.  This is an inherent signaling race condition
		 * and is not a bug.
		 *
		 * A child of vfork() is not allowed to change signal
		 * handlers to anything other than SIG_DFL or SIG_IGN.
		 */
		if (self->ul_vfork) {
			if (tact.sa_sigaction != SIG_IGN)
				tact.sa_sigaction = SIG_DFL;
		} else if (sig == SIGCANCEL || sig == SIGAIOCANCEL) {
			/*
			 * Always catch these signals.
			 * We need SIGCANCEL for pthread_cancel() to work.
			 * We need SIGAIOCANCEL for aio_cancel() to work.
			 */
			udp->siguaction[sig].sig_uaction = tact;
			if (tact.sa_sigaction == SIG_DFL ||
			    tact.sa_sigaction == SIG_IGN)
				tact.sa_flags = SA_SIGINFO;
			else {
				tact.sa_flags |= SA_SIGINFO;
				tact.sa_flags &= ~(SA_NODEFER | SA_RESETHAND);
			}
			tact.sa_sigaction = udp->sigacthandler;
			tact.sa_mask = maskset;
		} else if (tact.sa_sigaction != SIG_DFL &&
		    tact.sa_sigaction != SIG_IGN) {
			udp->siguaction[sig].sig_uaction = tact;
			tact.sa_flags &= ~SA_NODEFER;
			tact.sa_sigaction = udp->sigacthandler;
			tact.sa_mask = maskset;
		}
	}

	if ((rv = __sigaction(sig, tactp, oact)) != 0)
		udp->siguaction[sig].sig_uaction = oaction;
	else if (oact != NULL &&
	    oact->sa_sigaction != SIG_DFL &&
	    oact->sa_sigaction != SIG_IGN)
		*oact = oaction;

	/*
	 * We detect setting the disposition of SIGIO just to set the
	 * _sigio_enabled flag for the asynchronous i/o (aio) code.
	 */
	if (sig == SIGIO && rv == 0 && tactp != NULL) {
		_sigio_enabled =
		    (tactp->sa_handler != SIG_DFL &&
		    tactp->sa_handler != SIG_IGN);
	}

	if (!self->ul_vfork)
		lmutex_unlock(&udp->siguaction[sig].sig_lock);
	return (rv);
}

void
setsigacthandler(void (*nsigacthandler)(int, siginfo_t *, void *),
    void (**osigacthandler)(int, siginfo_t *, void *))
{
	ulwp_t *self = curthread;
	uberdata_t *udp = self->ul_uberdata;

	if (osigacthandler != NULL)
		*osigacthandler = udp->sigacthandler;

	udp->sigacthandler = nsigacthandler;
}

/*
 * Calling set_parking_flag(curthread, 1) informs the kernel that we are
 * calling __lwp_park or ___lwp_cond_wait().  If we take a signal in
 * the unprotected (from signals) interval before reaching the kernel,
 * sigacthandler() will call set_parking_flag(curthread, 0) to inform
 * the kernel to return immediately from these system calls, giving us
 * a spurious wakeup but not a deadlock.
 */
void
set_parking_flag(ulwp_t *self, int park)
{
	volatile sc_shared_t *scp;

	enter_critical(self);
	if ((scp = self->ul_schedctl) != NULL ||
	    (scp = setup_schedctl()) != NULL)
		scp->sc_park = park;
	else if (park == 0)	/* schedctl failed, do it the long way */
		__lwp_unpark(self->ul_lwpid);
	exit_critical(self);
}

/*
 * Tell the kernel to block all signals.
 * Use the schedctl interface, or failing that, use __lwp_sigmask().
 * This action can be rescinded only by making a system call that
 * sets the signal mask:
 *	__lwp_sigmask(), __sigprocmask(), __setcontext(),
 *	__sigsuspend() or __pollsys().
 * In particular, this action cannot be reversed by assigning
 * scp->sc_sigblock = 0.  That would be a way to lose signals.
 * See the definition of restore_signals(self).
 */
void
block_all_signals(ulwp_t *self)
{
	volatile sc_shared_t *scp;

	enter_critical(self);
	if ((scp = self->ul_schedctl) != NULL ||
	    (scp = setup_schedctl()) != NULL)
		scp->sc_sigblock = 1;
	else
		(void) __lwp_sigmask(SIG_SETMASK, &maskset, NULL);
	exit_critical(self);
}

/*
 * _private_setcontext has code that forcibly restores the curthread
 * pointer in a context passed to the setcontext(2) syscall.
 *
 * Certain processes may need to disable this feature, so these routines
 * provide the mechanism to do so.
 *
 * (As an example, branded 32-bit x86 processes may use %gs for their own
 * purposes, so they need to be able to specify a %gs value to be restored
 * on return from a signal handler via the passed ucontext_t.)
 */
static int setcontext_enforcement = 1;

void
set_setcontext_enforcement(int on)
{
	setcontext_enforcement = on;
}

#pragma weak setcontext = _private_setcontext
#pragma weak _setcontext = _private_setcontext
int
_private_setcontext(const ucontext_t *ucp)
{
	ulwp_t *self = curthread;
	int ret;
	ucontext_t uc;

	/*
	 * Returning from the main context (uc_link == NULL) causes
	 * the thread to exit.  See setcontext(2) and makecontext(3C).
	 */
	if (ucp == NULL)
		_thr_exit(NULL);
	(void) _private_memcpy(&uc, ucp, sizeof (uc));

	/*
	 * Restore previous signal mask and context link.
	 */
	if (uc.uc_flags & UC_SIGMASK) {
		block_all_signals(self);
		delete_reserved_signals(&uc.uc_sigmask);
		self->ul_sigmask = uc.uc_sigmask;
		if (self->ul_cursig) {
			/*
			 * We have a deferred signal present.
			 * The signal mask will be set when the
			 * signal is taken in take_deferred_signal().
			 */
			ASSERT(self->ul_critical + self->ul_sigdefer != 0);
			uc.uc_flags &= ~UC_SIGMASK;
		}
	}
	self->ul_siglink = uc.uc_link;

	/*
	 * We don't know where this context structure has been.
	 * Preserve the curthread pointer, at least.
	 *
	 * Allow this feature to be disabled if a particular process
	 * requests it.
	 */
	if (setcontext_enforcement) {
#if defined(__sparc)
		uc.uc_mcontext.gregs[REG_G7] = (greg_t)self;
#elif defined(__amd64)
		uc.uc_mcontext.gregs[REG_FS] = (greg_t)self->ul_gs;
#elif defined(__i386)
		uc.uc_mcontext.gregs[GS] = (greg_t)self->ul_gs;
#else
#error "none of __sparc, __amd64, __i386 defined"
#endif
	}

	/*
	 * Make sure that if we return to a call to __lwp_park()
	 * or ___lwp_cond_wait() that it returns right away
	 * (giving us a spurious wakeup but not a deadlock).
	 */
	set_parking_flag(self, 0);
	self->ul_sp = 0;
	ret = __setcontext_syscall(&uc);

	/*
	 * It is OK for setcontext() to return if the user has not specified
	 * UC_CPU.
	 */
	if (uc.uc_flags & UC_CPU)
		thr_panic("setcontext(): __setcontext() returned");
	return (ret);
}

#pragma weak thr_sigsetmask = _thr_sigsetmask
#pragma weak pthread_sigmask = _thr_sigsetmask
#pragma weak _pthread_sigmask = _thr_sigsetmask
int
_thr_sigsetmask(int how, const sigset_t *set, sigset_t *oset)
{
	ulwp_t *self = curthread;
	sigset_t saveset;

	if (set == NULL) {
		enter_critical(self);
		if (oset != NULL)
			*oset = self->ul_sigmask;
		exit_critical(self);
	} else {
		switch (how) {
		case SIG_BLOCK:
		case SIG_UNBLOCK:
		case SIG_SETMASK:
			break;
		default:
			return (EINVAL);
		}

		/*
		 * The assignments to self->ul_sigmask must be protected from
		 * signals.  The nuances of this code are subtle.  Be careful.
		 */
		block_all_signals(self);
		if (oset != NULL)
			saveset = self->ul_sigmask;
		switch (how) {
		case SIG_BLOCK:
			self->ul_sigmask.__sigbits[0] |= set->__sigbits[0];
			self->ul_sigmask.__sigbits[1] |= set->__sigbits[1];
			break;
		case SIG_UNBLOCK:
			self->ul_sigmask.__sigbits[0] &= ~set->__sigbits[0];
			self->ul_sigmask.__sigbits[1] &= ~set->__sigbits[1];
			break;
		case SIG_SETMASK:
			self->ul_sigmask.__sigbits[0] = set->__sigbits[0];
			self->ul_sigmask.__sigbits[1] = set->__sigbits[1];
			break;
		}
		delete_reserved_signals(&self->ul_sigmask);
		if (oset != NULL)
			*oset = saveset;
		restore_signals(self);
	}

	return (0);
}

#pragma weak sigprocmask = _sigprocmask
int
_sigprocmask(int how, const sigset_t *set, sigset_t *oset)
{
	int error;

	/*
	 * Guard against children of vfork().
	 */
	if (curthread->ul_vfork)
		return (__lwp_sigmask(how, set, oset));

	if ((error = _thr_sigsetmask(how, set, oset)) != 0) {
		errno = error;
		return (-1);
	}

	return (0);
}

/*
 * Called at library initialization to set up signal handling.
 * All we really do is initialize the sig_lock mutexes.
 * All signal handlers are either SIG_DFL or SIG_IGN on exec().
 * However, if any signal handlers were established on alternate
 * link maps before the primary link map has been initialized,
 * then inform the kernel of the new sigacthandler.
 */
void
signal_init()
{
	uberdata_t *udp = curthread->ul_uberdata;
	struct sigaction *sap;
	struct sigaction act;
	int sig;

	for (sig = 0; sig < NSIG; sig++) {
		udp->siguaction[sig].sig_lock.mutex_magic = MUTEX_MAGIC;
		sap = &udp->siguaction[sig].sig_uaction;
		if (sap->sa_sigaction != SIG_DFL &&
		    sap->sa_sigaction != SIG_IGN &&
		    __sigaction(sig, NULL, &act) == 0 &&
		    act.sa_sigaction != SIG_DFL &&
		    act.sa_sigaction != SIG_IGN) {
			act = *sap;
			act.sa_flags &= ~SA_NODEFER;
			act.sa_sigaction = udp->sigacthandler;
			act.sa_mask = maskset;
			(void) __sigaction(sig, &act, NULL);
		}
	}
}

/*
 * Common code for cancelling self in _sigcancel() and pthread_cancel().
 * If the thread is at a cancellation point (ul_cancelable) then just
 * return and let _canceloff() do the exit, else exit immediately if
 * async mode is in effect.
 */
void
do_sigcancel()
{
	ulwp_t *self = curthread;

	ASSERT(self->ul_critical == 0);
	ASSERT(self->ul_sigdefer == 0);
	self->ul_cancel_pending = 1;
	if (self->ul_cancel_async &&
	    !self->ul_cancel_disabled &&
	    !self->ul_cancelable)
		_pthread_exit(PTHREAD_CANCELED);
}

/*
 * Set up the SIGCANCEL handler for threads cancellation,
 * needed only when we have more than one thread,
 * or the SIGAIOCANCEL handler for aio cancellation,
 * called when aio is initialized, in __uaio_init().
 */
void
setup_cancelsig(int sig)
{
	uberdata_t *udp = curthread->ul_uberdata;
	mutex_t *mp = &udp->siguaction[sig].sig_lock;
	struct sigaction act;

	ASSERT(sig == SIGCANCEL || sig == SIGAIOCANCEL);
	lmutex_lock(mp);
	act = udp->siguaction[sig].sig_uaction;
	lmutex_unlock(mp);
	if (act.sa_sigaction == SIG_DFL ||
	    act.sa_sigaction == SIG_IGN)
		act.sa_flags = SA_SIGINFO;
	else {
		act.sa_flags |= SA_SIGINFO;
		act.sa_flags &= ~(SA_NODEFER | SA_RESETHAND);
	}
	act.sa_sigaction = udp->sigacthandler;
	act.sa_mask = maskset;
	(void) __sigaction(sig, &act, NULL);
}
