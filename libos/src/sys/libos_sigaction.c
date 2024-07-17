/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University
 * Copyright (C) 2020 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */

/*
 * Implementation of system calls "sigaction", "sigreturn", "sigprocmask", "kill", "tkill"
 * and "tgkill".
 */

#include <limits.h>

#include "libos_internal.h"
#include "libos_ipc.h"
#include "libos_lock.h"
#include "libos_process.h"
#include "libos_rwlock.h"
#include "libos_table.h"
#include "libos_thread.h"
#include "libos_utils.h"
#include "linux_abi/errors.h"
#include "linux_abi/signals.h"
#include "pal.h"

long libos_syscall_rt_sigaction(int signum, const struct __kernel_sigaction* act,
                                struct __kernel_sigaction* oldact, size_t sigsetsize) {
    /* SIGKILL and SIGSTOP cannot be caught or ignored */
    if (signum == SIGKILL || signum == SIGSTOP || signum <= 0 || signum > SIGS_CNT ||
            sigsetsize != sizeof(__sigset_t))
        return -EINVAL;

    if (act && !is_user_memory_readable(act, sizeof(*act)))
        return -EFAULT;

    if (oldact && !is_user_memory_writable(oldact, sizeof(*oldact)))
        return -EFAULT;

#if defined(__x86_64__)
    if (act && !(act->sa_flags & SA_RESTORER)) {
        log_warning("rt_sigaction: SA_RESTORER flag is required!");
        return -EINVAL;
    }
#endif

    struct libos_thread* cur = get_cur_thread();

    lock(&cur->signal_dispositions->lock);

    struct __kernel_sigaction* sigaction = &cur->signal_dispositions->actions[signum - 1];

    if (oldact)
        *oldact = *sigaction;

    if (act)
        *sigaction = *act;

    clear_illegal_signals(&sigaction->sa_mask);

    unlock(&cur->signal_dispositions->lock);
    return 0;
}

long libos_syscall_rt_sigreturn(void) {
    PAL_CONTEXT* context = LIBOS_TCB_GET(context.regs);
    void* orig_rip = (void*)pal_context_get_ip(context);

    __sigset_t new_mask;
    restore_sigreturn_context(context, &new_mask);
    clear_illegal_signals(&new_mask);

    /* FIXME: make this VM/TDX PAL logic more generic */
    if (!strcmp(g_pal_public_state->host_type, "VM") ||
            !strcmp(g_pal_public_state->host_type, "TDX")) {
        /*
         * Rewire context RIP to the VM/TDX PAL sysret trampoline:
         *   - orig_rip contains the RIP of the sysret trampoline code (see PAL's kernel_events.S)
         *   - restored rip contains the RIP of the where-to-return app code
         */
        void* restored_rip = (void*)pal_context_get_ip(context);
        void* user_rip_ptr = (char*)pal_get_tcb() + g_pal_public_state->vm_user_rip_offset;
        memcpy(user_rip_ptr, &restored_rip, sizeof(restored_rip));
        pal_context_set_ip(context, (uintptr_t)orig_rip);
    }

    struct libos_thread* current = get_cur_thread();
    lock(&current->lock);
    set_sig_mask(current, &new_mask);
    unlock(&current->lock);

    /* We restored user context, it's not a syscall. */
    LIBOS_TCB_SET(context.syscall_nr, -1);

    return pal_context_get_retval(context);
}

long libos_syscall_rt_sigprocmask(int how, const __sigset_t* set, __sigset_t* oldset,
                                  size_t sigsetsize) {
    __sigset_t old;

    if (sigsetsize != sizeof(*set))
        return -EINVAL;

    if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK)
        return -EINVAL;

    if (set && !is_user_memory_readable(set, sizeof(*set)))
        return -EFAULT;

    if (oldset && !is_user_memory_readable(oldset, sizeof(*oldset)))
        return -EFAULT;

    struct libos_thread* cur = get_cur_thread();

    lock(&cur->lock);

    get_sig_mask(cur, &old);

    if (oldset) {
        *oldset = old;
    }

    /* If set is NULL, then the signal mask is unchanged. */
    if (!set)
        goto out;

    switch (how) {
        case SIG_BLOCK:
            __sigorset(&old, &old, set);
            break;

        case SIG_UNBLOCK:
            __signotset(&old, &old, set);
            break;

        case SIG_SETMASK:
            old = *set;
            break;
    }

    clear_illegal_signals(&old);
    set_sig_mask(cur, &old);

out:
    unlock(&cur->lock);

    return 0;
}

long libos_syscall_sigaltstack(const stack_t* ss, stack_t* oss) {
    if (ss && !is_user_memory_readable(ss, sizeof(*ss))) {
        return -EFAULT;
    }
    if (oss && !is_user_memory_writable(oss, sizeof(*oss))) {
        return -EFAULT;
    }

    if (ss && (ss->ss_flags & ~SS_DISABLE))
        return -EINVAL;

    struct libos_thread* cur = get_cur_thread();

    stack_t* cur_ss = &cur->signal_altstack;

    if (oss) {
        *oss = *cur_ss;
        if (cur_ss->ss_size == 0) {
            oss->ss_flags |= SS_DISABLE;
        }
    }

    if (!(cur_ss->ss_flags & SS_DISABLE)
            && is_on_altstack(pal_context_get_sp(libos_get_tcb()->context.regs), cur_ss)) {
        /* We are currently using the alternative stack. */
        if (oss)
            oss->ss_flags |= SS_ONSTACK;
        if (ss) {
            return -EPERM;
        }
    }

    if (ss) {
        if (ss->ss_flags & SS_DISABLE) {
            memset(cur_ss, 0, sizeof(*cur_ss));
            cur_ss->ss_flags = SS_DISABLE;
        } else {
            if (ss->ss_size < MINSIGSTKSZ) {
                return -ENOMEM;
            }

            *cur_ss = *ss;
        }
    }

    return 0;
}

long libos_syscall_rt_sigsuspend(const __sigset_t* mask_ptr, size_t setsize) {
    int ret = set_user_sigmask(mask_ptr, setsize);
    if (ret < 0) {
        return ret;
    }

    thread_prepare_wait();
    while (!have_pending_signals()) {
        ret = thread_wait(/*timeout_us=*/NULL, /*ignore_pending_signals=*/false);
        if (ret < 0 && ret != -EINTR) {
            return ret;
        }
    }

    return -EINTR;
}

long libos_syscall_rt_sigtimedwait(const __sigset_t* unblocked_ptr, siginfo_t* info,
                                   struct __kernel_timespec* timeout, size_t setsize) {
    int ret;

    if (setsize != sizeof(sigset_t)) {
        return -EINVAL;
    }
    if (!is_user_memory_readable(unblocked_ptr, sizeof(*unblocked_ptr))) {
        return -EFAULT;
    }
    if (info && !is_user_memory_writable(info, sizeof(*info))) {
        return -EFAULT;
    }

    if (timeout) {
        if (!is_user_memory_readable(timeout, sizeof(*timeout))) {
            return -EFAULT;
        }
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
                (uint64_t)timeout->tv_nsec >= TIME_NS_IN_S) {
            return -EINVAL;
        }
    }

    __sigset_t unblocked = *unblocked_ptr;
    clear_illegal_signals(&unblocked);

    /* Note that the user of `rt_sigtimedwait()` is supposed to block the signals in `unblocked` set
     * via a prior call to `sigprocmask()`, so that these signals can only occur as a response to
     * `rt_sigtimedwait()`. Temporarily augment the current mask with these unblocked signals. */
    __sigset_t new;
    __sigset_t old;

    struct libos_thread* current = get_cur_thread();
    lock(&current->lock);
    get_sig_mask(current, &old);
    __signotset(&new, &old, &unblocked);
    set_sig_mask(current, &new);
    unlock(&current->lock);

    uint64_t timeout_us = timeout ? timespec_to_us(timeout) : 0;
    int thread_wait_res = -EINTR;

    thread_prepare_wait();
    while (!have_pending_signals()) {
        thread_wait_res = thread_wait(timeout ? &timeout_us : NULL,
                                      /*ignore_pending_signals=*/false);
        if (thread_wait_res == -ETIMEDOUT) {
            break;
        }
    }

    /* If `have_pending_signals()` spotted a signal, we just pray it was targeted directly at this
     * thread or no other thread handles it first; see also `do_nanosleep()` in libos_sleep.c */

    /* invert the set of unblocked signals to get the mask for popping one of requested signals */
    __sigset_t all_blocked;
    __sigfillset(&all_blocked);
    __sigset_t mask;
    __signotset(&mask, &all_blocked, &unblocked);

    struct libos_signal signal = { 0 };
    pop_unblocked_signal(&mask, &signal);

    if (signal.siginfo.si_signo) {
        if (info) {
            *info = signal.siginfo;
        }
        ret = signal.siginfo.si_signo;
    } else {
        ret = (thread_wait_res == -ETIMEDOUT ? -EAGAIN : -EINTR);
    }

    lock(&current->lock);
    set_sig_mask(current, &old);
    unlock(&current->lock);

    return ret;
}

long libos_syscall_rt_sigpending(__sigset_t* set, size_t sigsetsize) {
    if (sigsetsize != sizeof(*set))
        return -EINVAL;

    if (!is_user_memory_writable(set, sigsetsize))
        return -EFAULT;

    get_all_pending_signals(set);

    struct libos_thread* current = get_cur_thread();
    /* We are interested only in blocked signals... */
    lock(&current->lock);
    __sigandset(set, set, &current->signal_mask);
    unlock(&current->lock);

    /* ...and not ignored. */
    lock(&current->signal_dispositions->lock);
    for (int sig = 1; sig <= SIGS_CNT; sig++) {
        if (current->signal_dispositions->actions[sig - 1].k_sa_handler == SIG_IGN) {
            __sigdelset(set, sig);
        }
    }
    unlock(&current->signal_dispositions->lock);

    return 0;
}

int kill_current_proc(siginfo_t* info) {
    if (!info->si_signo) {
        return 0;
    }

    int ret = append_signal(NULL, info);
    if (ret < 0) {
        return ret;
    }

    int sig = info->si_signo;
    struct libos_thread* current = get_cur_thread();
    if (!is_internal(current)) {
        /* Can we handle this signal? */
        lock(&current->lock);
        if (!__sigismember(&current->signal_mask, sig)) {
            /* Yes we can. */
            unlock(&current->lock);
            return 0;
        }
        unlock(&current->lock);
    }

    ret = walk_thread_list(wakeup_one_thread_on_signal, (void*)(long)sig, /*one_shot=*/true);
    /* Ignore `-ESRCH` as this just means that currently no thread is able to handle the signal. */
    if (ret < 0 && ret != -ESRCH) {
        return ret;
    }

    return 0;
}

int do_kill_proc(IDTYPE sender, IDTYPE pid, int sig) {
    if (g_process.pid != pid) {
        return ipc_kill_process(g_process.pid, pid, sig);
    }

    siginfo_t info = {
        .si_signo = sig,
        .si_pid   = sender,
        .si_code  = SI_USER
    };
    return kill_current_proc(&info);
}

int do_kill_pgroup(IDTYPE sender, IDTYPE pgid, int sig) {
    rwlock_read_lock(&g_process_id_lock);
    IDTYPE current_pgid = g_process.pgid;
    rwlock_read_unlock(&g_process_id_lock);
    if (!pgid) {
        pgid = current_pgid;
    }

    /* TODO: currently process groups are not supported. */
#if 0
    int ret = ipc_kill_pgroup(sender, pgid, sig);
    if (ret < 0 && ret != -ESRCH) {
        return ret;
    }
#else
    int ret = -ENOSYS;
#endif

    if (current_pgid != pgid) {
        return ret;
    }

    siginfo_t info = {
        .si_signo = sig,
        .si_pid   = sender,
        .si_code  = SI_USER
    };
    return kill_current_proc(&info);
}

long libos_syscall_kill(pid_t pid, int sig) {
    if (sig < 0 || sig > SIGS_CNT) {
        return -EINVAL;
    }

    if (pid == INT_MIN) {
        /* We should not negate INT_MIN. */
        return -ESRCH;
    }

    if (pid > 0) {
        /* If `pid` is positive, then signal is sent to the process with that pid. */
        return do_kill_proc(g_process.pid, pid, sig);
    } else if (pid == -1) {
        /* If `pid` equals -1, then signal is sent to every process for which the calling process
         * has permission to send, which means all processes in Gramine. NOTE: On Linux, kill(-1)
         * does not signal the calling process. */
        return ipc_kill_all(g_process.pid, sig);
    } else if (pid == 0) {
        /* If `pid` equals 0, then signal is sent to every process in the process group of
         * the calling process. */
        return do_kill_pgroup(g_process.pid, 0, sig);
    } else { // pid < -1
        /* If `pid` is less than -1, then signal is sent to every process in the process group
         * `-pid`. */
        return do_kill_pgroup(g_process.pid, -pid, sig);
    }
}

int do_kill_thread(IDTYPE sender, IDTYPE tgid, IDTYPE tid, int sig) {
    if (sig < 0 || sig > SIGS_CNT)
        return -EINVAL;

    if (g_process.pid != tgid) {
        return ipc_kill_thread(sender, tgid, tid, sig);
    }

    struct libos_thread* thread = lookup_thread(tid);
    if (!thread) {
        return -ESRCH;
    }

    if (!sig) {
        put_thread(thread);
        return 0;
    }

    siginfo_t info = {
        .si_signo = sig,
        .si_pid   = sender,
        .si_code  = SI_TKILL,
    };
    int ret = append_signal(thread, &info);
    if (ret < 0) {
        put_thread(thread);
        return ret;
    }
    if (thread != get_cur_thread()) {
        thread_wakeup(thread);
        ret = pal_to_unix_errno(PalThreadResume(thread->pal_handle));
    }

    put_thread(thread);
    return ret;
}

long libos_syscall_tkill(pid_t tid, int sig) {
    if (tid <= 0)
        return -EINVAL;

    /* `tkill` is obsolete, so we do not support using it to kill threads in different process. */
    return do_kill_thread(g_process.pid, g_process.pid, tid, sig);
}

long libos_syscall_tgkill(pid_t tgid, pid_t tid, int sig) {
    if (tgid <= 0 || tid <= 0)
        return -EINVAL;

    return do_kill_thread(g_process.pid, tgid, tid, sig);
}

void fill_siginfo_code_and_status(siginfo_t* info, int signal, int exit_code) {
    if (signal == 0) {
        info->si_code = CLD_EXITED;
        info->si_status = exit_code;
    } else if (signal & __WCOREDUMP_BIT) {
        info->si_code = CLD_DUMPED;
        info->si_status = signal & ~__WCOREDUMP_BIT;
    } else {
        info->si_code = CLD_KILLED;
        info->si_status = signal;
    }
}
