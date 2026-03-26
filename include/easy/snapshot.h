#ifndef EASY_SNAPSHOT
#define EASY_SNAPSHOT

#include <cstddef>
#include <memory>
#include <type_traits>

namespace easy {

template<class T>
struct snapshot_value {
  using value_type = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>;
  const value_type* ptr;
};

template<class T>
struct snapshot_array_value {
  using value_type = std::remove_cv_t<T>;
  const value_type* data;
  size_t count;
};

template<class T>
std::enable_if_t<!std::is_pointer<std::decay_t<T>>::value, snapshot_value<T>>
snapshot(T const& value) {
  using value_type = typename snapshot_value<T>::value_type;
  static_assert(std::is_class<value_type>::value,
                "easy::snapshot currently supports struct/class objects");
  static_assert(std::is_trivially_copyable<value_type>::value,
                "easy::snapshot requires trivially copyable objects");
  return snapshot_value<T>{std::addressof(value)};
}

template<class T>
std::enable_if_t<!std::is_pointer<T>::value, snapshot_value<T*>>
snapshot(T* value) {
  using value_type = typename snapshot_value<T*>::value_type;
  static_assert(std::is_class<value_type>::value,
                "easy::snapshot currently supports struct/class objects");
  static_assert(std::is_trivially_copyable<value_type>::value,
                "easy::snapshot requires trivially copyable objects");
  return snapshot_value<T*>{value};
}

template<class T>
std::enable_if_t<!std::is_pointer<T>::value, snapshot_value<const T*>>
snapshot(T const* value) {
  using value_type = typename snapshot_value<const T*>::value_type;
  static_assert(std::is_class<value_type>::value,
                "easy::snapshot currently supports struct/class objects");
  static_assert(std::is_trivially_copyable<value_type>::value,
                "easy::snapshot requires trivially copyable objects");
  return snapshot_value<const T*>{value};
}

template<class T, size_t N>
snapshot_array_value<T> snapshot_array(T const (&data)[N]) {
  static_assert(std::is_trivially_copyable<T>::value,
                "easy::snapshot_array requires trivially copyable elements");
  return snapshot_array_value<T>{data, N};
}

template<class T>
snapshot_array_value<T> snapshot_array(T const* data, size_t count) {
  static_assert(std::is_trivially_copyable<T>::value,
                "easy::snapshot_array requires trivially copyable elements");
  return snapshot_array_value<T>{data, count};
}

template<class T>
struct is_snapshot_value : std::false_type {};

template<class T>
struct is_snapshot_value<snapshot_value<T>> : std::true_type {};

template<class T>
struct is_snapshot_array_value : std::false_type {};

template<class T>
struct is_snapshot_array_value<snapshot_array_value<T>> : std::true_type {};

}

#endif