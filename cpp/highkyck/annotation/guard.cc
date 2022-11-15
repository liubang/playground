#include <mutex>
#include <string>
#include <unordered_map>

// Enable thread safety attributes only with clang.
// The attributes can be safely erased when compiling with other compilers.
#if defined(__clang__) && (!defined(SWIG))
#define THREAD_ANNOTATION_ATTRIBUTE__(x) __attribute__((x))
#else
#define THREAD_ANNOTATION_ATTRIBUTE__(x)  // no-op
#endif

#define GUARDED_BY(x) THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))

namespace highkyck::annotation {

class Foo {
 public:
  void set(const std::string& key, const std::string& val) {
    std::unique_lock<std::mutex> lk(mutex_);
    map_[key] = val;
  }

  std::string get(const std::string& key) {
    std::unique_lock<std::mutex> lk(mutex_);
    return map_[key];
  }

 private:
  std::mutex mutex_;
  std::unordered_map<std::string, std::string> map_ GUARDED_BY(mutex_);
};

}  // namespace highkyck::annotation

int main(int argc, char* argv[]) {
  highkyck::annotation::Foo foo;
  foo.set("hello", "world");
  return 0;
}
