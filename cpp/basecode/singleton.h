#pragma once

#include <pthread.h>

namespace basecode
{

template<typename T>
class Singleton
{
public:
  static T* get_instance()
  {
    pthread_once(&once_control_, &Singleton::init);
    return value_;
  }

  static void destroy()
  {
    if (value_ != nullptr) { delete value_; }
  }

private:
  Singleton();

  ~Singleton();

  static void init()
  {
    value_ = new T();
    atexit(destroy);
  }

private:
  static T* value_;
  static pthread_once_t once_control_;
};

template<typename T>
pthread_once_t Singleton<T>::once_control_ = PTHREAD_ONCE_INIT;

template<typename T>
T* Singleton<T>::value_ = nullptr;

}  // namespace basecode
