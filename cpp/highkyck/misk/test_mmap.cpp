#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>

#include <iostream>

int main(int argc, char* argv[]) {
  const char* file = "/tmp/test.bin";
  int fd = ::open(file, O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600);
  if (fd < 0) {
    std::cerr << "failed to open file " << file << std::endl;
    return -1;
  }

  const char* text = "this is test contents";
  std::size_t len = strlen(text) + 1;
  if (lseek(fd, len - 1, SEEK_SET) == -1) {
    close(fd);
    std::cerr << "failed to lseek" << std::endl;
    return -1;
  }

  if (write(fd, "", 1) == -1) {
    close(fd);
    std::cerr << "failed to write file " << file << std::endl;
    return -1;
  }

  void* map = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    close(fd);
    std::cerr << "failed to mmap" << std::endl;
    return -1;
  }

  char* charmap = (char*)map;
  for (std::size_t i = 0; i < len; ++i) {
    charmap[i] = text[i];
  }

  // write it now to disk
  if (msync(charmap, len, MS_SYNC) == -1) {
    std::cerr << "failed to msync" << std::endl;
  }

  if (munmap(charmap, len) == -1) {
    std::cerr << "failed to munmap" << std::endl;
    close(fd);
    return -1;
  }

  close(fd);

  return 0;
}
