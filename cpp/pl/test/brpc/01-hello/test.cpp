#ifdef __linux__
#include <bthread/bthread.h>

void* PrintHellobRPC(void* arg) {
    printf("I Love bRPC");
    return nullptr;
}

int main(int argc, char** argv) {
    bthread_t th_1;
    bthread_start_background(&th_1, nullptr, PrintHellobRPC, nullptr);
    bthread_join(th_1, nullptr);
    return 0;
}
#else

#include <iostream>
int main(int argc, char* argv[]) {
    std::cout << "hello world\n";
    return 0;
}

#endif
