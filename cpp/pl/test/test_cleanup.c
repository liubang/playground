// Copyright (c) 2025 The Authors. All rights reserved.
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

#include <stdio.h>

void clean(int* a) {
    // execute cleanup
    printf("clean up function... %d\n", *a);
}

int main(int argc, char* argv[]) {
    int a __attribute__((cleanup(clean))) = 1;
    int b __attribute__((cleanup(clean))) = 2;
    int c __attribute__((cleanup(clean))) = 3;
    int d __attribute__((cleanup(clean))) = 4;

    return 0;
}
