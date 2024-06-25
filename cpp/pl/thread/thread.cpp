// Copyright (c) 2024 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

#include "cpp/pl/thread/thread.h"

#if defined(__APPLE__) && defined(__MACH__)
#include <pthread.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace pl {

long gettid() {
    thread_local long tid = 0;
    if (tid == 0) {
#if defined(__APPLE__) && defined(__MACH__)
        pthread_threadid_np(nullptr, &tid);
#else
        tid = ::syscall(SYS_gettid);
#endif
    }
    return tid;
}

} // namespace pl
