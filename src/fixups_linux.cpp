/*
 * Copyright 2021 Andrei Pangin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <ucontext.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <sys/syscall.h>

#include "fixups_linux.h"

#define ARRAY_SIZE(arr)  (sizeof(arr) / sizeof(arr[0]))


bool Fixups::overriden_signal_action = false;
void (*Fixups::jvm_signal_action)(int sig, siginfo_t* info, void* uc);

struct fixup Fixups::fixup_arr[128];

bool Fixups::handleFixup(int sig, siginfo_t* info, void* uc) {
    // SIGSEGV is handled on the same thread (per what I understand)
    const pid_t tid = syscall(__NR_gettid);

    struct fixup *f = NULL;
    for (int i = 0; i < ARRAY_SIZE(fixup_arr); i++) {
        if (fixup_arr[i].tid == tid) {
            f = &fixup_arr[i];
        }
    }

    if (!f) {
        // no fixup block for it - fault is not ours.
        return false;
    }

    // copy from the saved context, to the signal context.
    // we copy all non-volatile registers (these are also the ones saved in setjmp)
    // can't use setcontext here because we don't want to "jump" into the saved context, only
    // restore what's needed into the context saved by the signal handler.
    greg_t *gregs = ((ucontext_t*)uc)->uc_mcontext.gregs;
    gregs[REG_RBX] = f->uc.uc_mcontext.gregs[REG_RBX];
    gregs[REG_RBP] = f->uc.uc_mcontext.gregs[REG_RBP];
    gregs[REG_R12] = f->uc.uc_mcontext.gregs[REG_R12];
    gregs[REG_R13] = f->uc.uc_mcontext.gregs[REG_R13];
    gregs[REG_R14] = f->uc.uc_mcontext.gregs[REG_R14];
    gregs[REG_R15] = f->uc.uc_mcontext.gregs[REG_R15];
    gregs[REG_RSP] = f->uc.uc_mcontext.gregs[REG_RSP];
    gregs[REG_RIP] = f->uc.uc_mcontext.gregs[REG_RIP];

    // let it know a fault was triggered (unlike setjmp/longjmp, the pair getcontext/setcontext
    // does not provide a "return value" from the getcontext function, so we have to do the book-keeping
    // ourselves here).
    f->triggered = true;

    // fault was handled
    return true;
}

struct fixup *Fixups::beginFixup() {
    const pid_t tid = syscall(__NR_gettid);

    for (int i = 0; i < ARRAY_SIZE(fixup_arr); i++) {
        if (__sync_bool_compare_and_swap(&fixup_arr[i].tid, 0, tid)) {
            fixup_arr[i].triggered = false;
            return &fixup_arr[i];
        }
    }

    return NULL; // TODO handle
}

void Fixups::endFixup(struct fixup *f) {
    const pid_t tid = syscall(__NR_gettid);

    assert(tid == f->tid);

    f->tid = 0;
}

void Fixups::fixupsSignalAction(int sig, siginfo_t* info, void* uc) {
    const int orig_errno = errno; // JVM saves errno

    if (!handleFixup(sig, info, uc)) {
        // If not handled as a fixup, call into the JVM handler.
        jvm_signal_action(sig, info, uc);
    }

    errno = orig_errno;
}

void Fixups::init() {
    // override the JVM's signal handler, just once.
    if (!overriden_signal_action) {
        struct sigaction act;
        sigaction(SIGSEGV, NULL, &act);
        jvm_signal_action = act.sa_sigaction;

        struct sigaction newact = act;
        newact.sa_sigaction = fixupsSignalAction;
        sigaction(SIGSEGV, &newact, NULL);

        overriden_signal_action = true;
    }
}
