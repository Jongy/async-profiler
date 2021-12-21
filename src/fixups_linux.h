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

#ifndef _FIXUPS_LINUX_H
#define _FIXUPS_LINUX_H

#ifdef __linux__

#include <ucontext.h>


struct fixup {
    ucontext_t uc;
    pid_t tid;
    bool triggered;
};

class Fixups {
  private:
    static bool overriden_signal_action;
    static void (*jvm_signal_action)(int sig, siginfo_t* info, void* uc);
    static struct fixup fixup_arr[];
    static void fixupsSignalAction(int sig, siginfo_t* info, void* uc);
    static bool handleFixup(int sig, siginfo_t* info, void* uc);

  public:
    static void init();
    static struct fixup *beginFixup();
    static void endFixup(struct fixup *f);
};

#define FIXUP_BLOCK(code, error)                \
  do {                                          \
      struct fixup *__f = Fixups::beginFixup(); \
      getcontext(&__f->uc);                     \
      if (!__f->triggered) {                    \
          code;                                 \
          Fixups::endFixup(__f);                \
      }                                         \
      else {                                    \
          Fixups::endFixup(__f);                \
          error;                                \
      }                                         \
  } while (0)                                   \


#endif // __linux__

#endif
