/* -*- Mode: C ; c-basic-offset: 4 -*- */
/*
 * Copyright (C) 2012 Nedko Arnaudov
 *
 * print out a stack-trace when program segfaults
 *
 * Inspiration: sigsegv.c by Jaco Kroon
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>
 * or write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#if defined(HAVE_CONFIG_H)
# include "config.h"
#endif

/* These are "defined" for documentation purposes. They should come from the build system with or without config.h */
#if 0
# define SIGINFO_CPP_DEMANGLE    /* whether to attempt c++ demangle. if used, you must link with libstdc++ */
# define SIGINFO_AUTO_INIT       /* whether to use gcc constructor function for automatic initialization */
#endif

#if !defined(SIGINFO_TEST)
# if !defined siginfo_log
#  error "siginfo_log not defined"
# endif
#else
# define siginfo_log(fmt, args...) fprintf(stderr, fmt "\n", ##args);
#endif

/* dladdr() is a glibc extension */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include <memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <errno.h>

#define SIGINFO_MAX_BT_FRAMES 20

#if defined(SA_SIGINFO) && !defined(__arm__) && !defined(__ia64__) && !defined(__alpha__) && !defined (__FreeBSD_kernel__) && !defined (__sh__) && !defined(__APPLE__) && !defined(__aarch64__)
# define USE_UCONTEXT
# include <ucontext.h>
#endif

#if defined(__powerpc64__)
# define SIGINFO_REGISTER(ucontext, index) ((ucontext)->uc_mcontext.gp_regs[index])
#elif defined(__powerpc__)
# define SIGINFO_REGISTER(ucontext, index) ((ucontext)->uc_mcontext.uc_regs->gregs[index])
#elif defined(__sparc__) && defined(__arch64__)
# define SIGINFO_REGISTER(ucontext, index) ((ucontext)->uc_mcontext.mc_gregs[index])
#else
# define SIGINFO_REGISTER(ucontext, index) ((ucontext)->uc_mcontext.gregs[index])
#endif

#if defined(REG_RIP)
# define SIGINFO_IP_REG REG_RIP
# define SIGINFO_BP_REG REG_RBP
# define SIGINFO_REGFORMAT "%016llx"
# define UINT_PTR_TYPE unsigned long long
#elif defined(REG_EIP)
# define SIGINFO_IP_REG REG_EIP
# define SIGINFO_BP_REG REG_EBP
# define SIGINFO_REGFORMAT "%08lx"
# define UINT_PTR_TYPE unsigned long
#else
# define SIGINFO_REGFORMAT "%x"
# define SIGINFO_STACK_GENERIC
# define UINT_PTR_TYPE unsigned int
#endif

struct si_code_descriptor
{
    int code;
    const char * description;
};

#ifdef USE_UCONTEXT
static struct si_code_descriptor sig_ill_codes[] =
{
    { ILL_ILLOPC, "ILL_ILLOPC; Illegal opcode" },
    { ILL_ILLOPN, "ILL_ILLOPN; Illegal operand" },
    { ILL_ILLADR, "ILL_ILLADR; Illegal addressing mode" },
    { ILL_ILLTRP, "ILL_ILLTRP; Illegal trap" },
    { ILL_PRVOPC, "ILL_PRVOPC; Privileged opcode" },
    { ILL_PRVREG, "ILL_PRVREG; Privileged register" },
    { ILL_COPROC, "ILL_COPROC; Coprocessor error" },
    { ILL_BADSTK, "ILL_BADSTK; Internal stack error" },
    { 0, NULL }
};

static struct si_code_descriptor sig_fpe_codes[] =
{
    { FPE_INTDIV, "FPE_INTDIV; Integer divide by zero" },
    { FPE_INTOVF, "FPE_INTOVF; Integer overflow" },
    { FPE_FLTDIV, "FPE_FLTDIV; Floating-point divide by zero" },
    { FPE_FLTOVF, "FPE_FLTOVF; Floating-point overflow" },
    { FPE_FLTUND, "FPE_FLTUND; Floating-point underflow" },
    { FPE_FLTRES, "FPE_FLTRES; Floating-point inexact result" },
    { FPE_FLTINV, "FPE_FLTINV; Invalid floating-point operation" },
    { FPE_FLTSUB, "FPE_FLTSUB; Subscript out of range" },
    { 0, NULL }
};

static struct si_code_descriptor sig_segv_codes[] = {
    { SEGV_MAPERR, "SEGV_MAPERR; Address not mapped to object" },
    { SEGV_ACCERR, "SEGV_ACCERR; Invalid permissions for mapped object" },
    { 0, NULL }
};

static struct si_code_descriptor sig_bus_codes[] =
{
    { BUS_ADRALN, "BUS_ADRALN; Invalid address alignment" },
    { BUS_ADRERR, "BUS_ADRERR; Nonexistent physical address" },
    { BUS_OBJERR, "BUS_OBJERR; Object-specific hardware error" },
    { 0, NULL }
};

static struct si_code_descriptor sig_any_codes[] =
{
    { SI_USER,    "SI_USER; sent by kill, sigsend, raise" },
#if defined(SI_KERNEL)
    { SI_KERNEL,  "SI_KERNEL; sent by the kernel from somewhere" },
#endif
    { SI_QUEUE,   "SI_QUEUE; Signal sent by the sigqueue()" },
    { SI_TIMER,   "SI_TIMER; Signal generated by expiration of a timer set by timer_settime()" },
    { SI_ASYNCIO, "SI_ASYNCIO; Signal generated by completion of an asynchronous I/O request" },
    { SI_MESGQ,   "SI_MESGQ; Signal generated by arrival of a message on an empty message queue" },
#if defined(SI_ASYNCIO)
    { SI_ASYNCIO, "SI_ASYNCIO; sent by AIO completion" },
#endif
#if defined(SI_SIGIO)
    { SI_SIGIO, "SI_SIGIO; sent by queued SIGIO" },
#endif
#if defined(SI_TKILL)
    { SI_TKILL, "SI_TKILL; sent by tkill system call" },
#endif
#if defined(SI_DETHREAD)
    { SI_DETHREAD, "SI_DETHREAD; sent by execve() killing subsidiary threads" },
#endif
    { 0, NULL }
};
#else /* #ifdef USE_UCONTEXT */
# define sig_ill_codes  NULL
# define sig_fpe_codes  NULL
# define sig_segv_codes NULL
# define sig_bus_codes  NULL
#endif /* #ifdef USE_UCONTEXT */

struct signal_descriptor
{
    int signo;
    const char * descr;
    struct si_code_descriptor * codes;
    const char * msg;
};

static struct signal_descriptor signal_descriptors[] =
{
    { SIGILL,  "SIGILL",  sig_ill_codes,  "Illegal instruction" },
    { SIGFPE,  "SIGFPE",  sig_fpe_codes,  "Floating point exception" },
    { SIGSEGV, "SIGSEGV", sig_segv_codes, "Segmentation Fault" },
    { SIGBUS,  "SIGBUS",  sig_bus_codes,  "Bus error (bad memory access)" },
    { SIGABRT, "SIGABRT", NULL,           "Abort" },
};

/* ucontext is required for non-generic stack backtrace */
#if !defined(USE_UCONTEXT) && !defined(SIGINFO_STACK_GENERIC)
# define SIGINFO_STACK_GENERIC
#endif

#if defined(SIGINFO_STACK_GENERIC)

static void dump_stack(ucontext_t * ucontext)
{
    size_t i;
    void * bt[SIGINFO_MAX_BT_FRAMES];
    char ** strings;
    size_t sz;

    ((void)(ucontext));         /* unreferenced parameter */

    siginfo_log("Stack trace (generic):");

    sz = backtrace(bt, SIGINFO_MAX_BT_FRAMES);
    strings = backtrace_symbols(bt, sz);

    for (i = 0; i < sz; i++)
    {
        siginfo_log("%2zu: %s", i, strings[i]);
    }

    siginfo_log("End of stack trace");
}

#else  /* #if defined(SIGINFO_STACK_GENERIC) */

#if defined(SIGINFO_CPP_DEMANGLE)
char * __cxa_demangle(const char * __mangled_name, char * __output_buffer, size_t * __length, int * __status);
#endif

static void dump_stack(ucontext_t * ucontext)
{
    int frame;
    Dl_info dlinfo;
    void ** bp;
    void * ip;
    const char * symname;
#if defined(SIGINFO_CPP_DEMANGLE)
    int demangle_status;
    char * demangled_name;
#endif

    ip = (void *) SIGINFO_REGISTER(ucontext, SIGINFO_IP_REG);
    bp = (void **)SIGINFO_REGISTER(ucontext, SIGINFO_BP_REG);

    siginfo_log("Stack trace:");

    for (frame = 0; bp != NULL && ip != NULL; frame++, ip = bp[1], bp = (void **)bp[0])
    {
        //siginfo_log("IP=%p BP=%p", ip, bp);
        if (dladdr(ip, &dlinfo) == 0)
        {
            siginfo_log("%2d: [dladdr failed for %p]", frame, ip);
            continue;
        }

        symname = dlinfo.dli_sname;
#if defined(SIGINFO_CPP_DEMANGLE)
        demangled_name = __cxa_demangle(symname, NULL, 0, &demangle_status);
        if (demangle_status == 0 && demangled_name != NULL)
        {
            symname = demangled_name;
        }
#endif

        siginfo_log(
            "%2d: 0x" SIGINFO_REGFORMAT " <%s+%d> (%s)",
            frame,
            (UINT_PTR_TYPE)ip,
            symname,
            (int)(ip - dlinfo.dli_saddr),
            dlinfo.dli_fname);

#if defined(SIGINFO_NO_CPP_DEMANGLE)
        free(demangled_name);
#endif

        if (dlinfo.dli_sname && strcmp(dlinfo.dli_sname, "main") == 0) break;
    }

    siginfo_log("End of stack trace");
}

#endif  /* #if defined(SIGINFO_STACK_GENERIC) */

static struct signal_descriptor * lookup_signal_descriptor(int signo)
{
    size_t i;

    for (i = 0; i < sizeof(signal_descriptors) / sizeof(signal_descriptors[0]); i++)
    {
        if (signal_descriptors[i].signo == signo)
        {
            return signal_descriptors + i;
        }
    }

    return NULL;
}

#if defined(USE_UCONTEXT)
static const char * si_code_description_lookup(struct si_code_descriptor * descr_ptr, int si_code)
{
    while (descr_ptr->description != NULL)
    {
        if (descr_ptr->code == si_code)
        {
            return descr_ptr->description;
        }

        descr_ptr++;
    }

    return NULL;
}

static const char * si_code_description(struct signal_descriptor * descr_ptr, int si_code)
{
    const char * si_code_str = NULL;

    si_code_str = si_code_description_lookup(sig_any_codes, si_code);
    if (si_code_str != NULL)
    {
        return si_code_str;
    }

    if (descr_ptr != NULL && descr_ptr->codes != NULL)
    {
        return si_code_description_lookup(descr_ptr->codes, si_code);
    }

    return "unknown";
}
#endif  /* #if defined(USE_UCONTEXT) */

static void dump_siginfo(int signo, siginfo_t * info)
{
    struct signal_descriptor * descr_ptr;

    descr_ptr = lookup_signal_descriptor(signo);
    if (descr_ptr != NULL)
    {
        siginfo_log("%s! (%s)", descr_ptr->msg, descr_ptr->descr);
    }
    else
    {
        siginfo_log("Unknown bad signal %d catched!", signo);
    }

#if defined(USE_UCONTEXT)
    siginfo_log("info.si_signo = %d", info->si_signo);
    siginfo_log("info.si_errno = %d", info->si_errno);
    siginfo_log("info.si_code  = %d (%s)", info->si_code, si_code_description(descr_ptr, info->si_code));
    siginfo_log("info.si_addr  = %p", info->si_addr);
#else
    (void)info;                 /* unused parameter */
#endif
}

#if defined(USE_UCONTEXT)
#define REGISTER_NAME_CASE(x) case REG_ ## x: return #x
static const char * register_name(size_t i)
{
    switch (i)
    {
#if defined(REG_EIP)
        REGISTER_NAME_CASE(GS);
        REGISTER_NAME_CASE(FS);
        REGISTER_NAME_CASE(ES);
        REGISTER_NAME_CASE(DS);
        REGISTER_NAME_CASE(EDI);
        REGISTER_NAME_CASE(ESI);
        REGISTER_NAME_CASE(EBP);
        REGISTER_NAME_CASE(ESP);
        REGISTER_NAME_CASE(EBX);
        REGISTER_NAME_CASE(EDX);
        REGISTER_NAME_CASE(ECX);
        REGISTER_NAME_CASE(EAX);
        REGISTER_NAME_CASE(TRAPNO);
        REGISTER_NAME_CASE(ERR);
        REGISTER_NAME_CASE(EIP);
        REGISTER_NAME_CASE(CS);
        REGISTER_NAME_CASE(EFL);
        REGISTER_NAME_CASE(UESP);
        REGISTER_NAME_CASE(SS);
#endif
    }

    return NULL;
}

static void dump_registers(ucontext_t * ucontext)
{
    size_t index;
    const char * name;
    char buffer[64];

    for (index = 0; index < NGREG; index++)
    {
        name = register_name(index);
        if (name == NULL)
        {
            snprintf(buffer, sizeof(buffer), "reg[%02d]", (int)index);
            name = buffer;
        }

        siginfo_log("%6s = 0x" SIGINFO_REGFORMAT, name, (UINT_PTR_TYPE)SIGINFO_REGISTER(ucontext, index));
    }
}
#endif  /* #if defined(USE_UCONTEXT) */

static void signal_handler(int signum, siginfo_t * info, void * ptr)
{
    dump_siginfo(signum, info);
#if defined(USE_UCONTEXT)
    dump_registers(ptr);
#endif
    dump_stack(ptr);
    exit(-1);
}

#if defined(SIGINFO_AUTO_INIT)
static
#endif
int setup_siginfo(void)
{
    struct sigaction action;
    size_t i;

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = signal_handler;
#ifdef SA_SIGINFO
    action.sa_flags = SA_SIGINFO;
#endif

    for (i = 0; i < sizeof(signal_descriptors)  / sizeof(signal_descriptors[0]); i++)
    {
        if (sigaction(signal_descriptors[i].signo, &action, NULL) < 0)
        {
            siginfo_log("sigaction failed for signal %d. errno is %d (%s)", signal_descriptors[i].signo, errno, strerror(errno));
            return 0;
        }
    }

    return 1;
}

#if defined(SIGINFO_AUTO_INIT)
static void __attribute((constructor)) init(void)
{
    setup_siginfo();
}
#endif

#if defined(SIGINFO_TEST)

void crash_abort(void)
{
    abort();
}

void crash_access(void)
{
    /* why this doesnt cause FPE? */
    *((float *)123) = 1000.0 / 0.0;
}

void crash_call(void)
{
    ((void (*)(void))(0xDEADBEEF))();
}

static void (* tests [])(void) =
{
    crash_abort,
    crash_access,
    crash_call,
};

int main(int argc, char ** argv)
{
    unsigned int index;

    setup_siginfo();

    if (argc > 1)
    {
        index = atoi(argv[1]);
        if (index >= sizeof(tests) / sizeof(tests[0]))
        {
            siginfo_log("invalid index %d, using 0 instead", index);
            index = 0;
        }
    }
    else
    {
        index = 0;
    }

    tests[index]();

    return 0;
}
#endif
