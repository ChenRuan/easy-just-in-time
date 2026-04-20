#ifndef EASY_SNAPSHOT
#define EASY_SNAPSHOT

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace easy {

struct snapshot_array_binding {
  size_t offset;
  const void* data;
  size_t count;
  size_t element_size;
};

struct snapshot_field_binding {
  size_t offset;
  const void* data;
  size_t size;
};

template<class T>
struct snapshot_value {
  using value_type = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>;
  const value_type* ptr;
  std::vector<snapshot_field_binding> field_bindings;
  std::vector<snapshot_array_binding> array_bindings;
};

template<class T>
struct snapshot_array_value {
  using value_type = std::remove_cv_t<T>;
  const value_type* data;
  size_t count;
};

template<class Object, class Pointer>
struct snapshot_array_binding_builder {
  Pointer Object::* member;
  size_t count;
};

template<class Object, class Field>
struct snapshot_field_binding_builder {
  Field Object::* member;
};

template<class Object, class Field>
snapshot_field_binding
make_snapshot_field_binding(const Object* object,
                            snapshot_field_binding_builder<Object, Field> binding) {
  using field_type = std::remove_cv_t<Field>;
  static_assert(std::is_trivially_copyable<field_type>::value,
                "easy::bind_field requires a trivially copyable field");

  auto const *base = reinterpret_cast<unsigned char const*>(object);
  auto const *field = reinterpret_cast<unsigned char const*>(&(object->*(binding.member)));
  return snapshot_field_binding{
      static_cast<size_t>(field - base),
      &(object->*(binding.member)),
      sizeof(field_type)};
}

template<class Object, class Pointer>
snapshot_array_binding
make_snapshot_array_binding(const Object* object,
                            snapshot_array_binding_builder<Object, Pointer> binding) {
  using pointer_type = std::remove_cv_t<Pointer>;
  using element_type = std::remove_cv_t<std::remove_pointer_t<pointer_type>>;

  static_assert(std::is_pointer<pointer_type>::value,
                "easy::bind_array requires a pointer field");
  static_assert(std::is_trivially_copyable<element_type>::value,
                "easy::bind_array requires trivially copyable elements");

  auto member_ptr = object->*(binding.member);
  assert((member_ptr != nullptr || binding.count == 0) &&
         "easy::bind_array does not accept null data with non-zero count");

  auto const *base = reinterpret_cast<unsigned char const*>(object);
  auto const *field = reinterpret_cast<unsigned char const*>(&(object->*(binding.member)));
  return snapshot_array_binding{
      static_cast<size_t>(field - base),
      member_ptr,
      binding.count,
      sizeof(element_type)};
}

template<class Object, class Field>
void append_snapshot_binding(const Object* object,
                             std::vector<snapshot_field_binding>& field_bindings,
                             std::vector<snapshot_array_binding>&,
                             snapshot_field_binding_builder<Object, Field> binding) {
  field_bindings.push_back(make_snapshot_field_binding<Object>(object, binding));
}

template<class Object, class Pointer>
void append_snapshot_binding(const Object* object,
                             std::vector<snapshot_field_binding>&,
                             std::vector<snapshot_array_binding>& array_bindings,
                             snapshot_array_binding_builder<Object, Pointer> binding) {
  array_bindings.push_back(make_snapshot_array_binding<Object>(object, binding));
}

template<class T, class ... Bindings>
snapshot_value<T> make_snapshot_value(T const* value, Bindings&& ... bindings) {
  using value_type = typename snapshot_value<T>::value_type;
  std::vector<snapshot_field_binding> field_bindings;
  std::vector<snapshot_array_binding> array_bindings;
  field_bindings.reserve(sizeof...(Bindings));
  array_bindings.reserve(sizeof...(Bindings));
  using expand = int[];
  (void)expand{0, (append_snapshot_binding<value_type>(
                        value,
                        field_bindings,
                        array_bindings,
                        std::forward<Bindings>(bindings)),
                    0)...};
  return snapshot_value<T>{value, std::move(field_bindings), std::move(array_bindings)};
}

template<class Object, class Pointer>
snapshot_array_binding_builder<Object, Pointer>
bind_array(Pointer Object::* member, size_t count) {
  return snapshot_array_binding_builder<Object, Pointer>{member, count};
}

template<class Object, class Field>
snapshot_field_binding_builder<Object, Field>
bind_field(Field Object::* member) {
  return snapshot_field_binding_builder<Object, Field>{member};
}

template<class T, class ... Bindings>
std::enable_if_t<!std::is_pointer<std::decay_t<T>>::value, snapshot_value<T>>
snapshot(T const& value, Bindings&& ... bindings) {
  using value_type = typename snapshot_value<T>::value_type;
  static_assert(std::is_class<value_type>::value,
                "easy::snapshot currently supports struct/class objects");
  static_assert(std::is_trivially_copyable<value_type>::value,
                "easy::snapshot requires trivially copyable objects");
  return make_snapshot_value(std::addressof(value), std::forward<Bindings>(bindings)...);
}

template<class T, class ... Bindings>
std::enable_if_t<!std::is_pointer<T>::value, snapshot_value<T*>>
snapshot(T* value, Bindings&& ... bindings) {
  using value_type = typename snapshot_value<T*>::value_type;
  static_assert(std::is_class<value_type>::value,
                "easy::snapshot currently supports struct/class objects");
  static_assert(std::is_trivially_copyable<value_type>::value,
                "easy::snapshot requires trivially copyable objects");
  assert(value != nullptr && "easy::snapshot does not accept null pointers");
  return make_snapshot_value(value, std::forward<Bindings>(bindings)...);
}

template<class T, class ... Bindings>
std::enable_if_t<!std::is_pointer<T>::value, snapshot_value<const T*>>
snapshot(T const* value, Bindings&& ... bindings) {
  using value_type = typename snapshot_value<const T*>::value_type;
  static_assert(std::is_class<value_type>::value,
                "easy::snapshot currently supports struct/class objects");
  static_assert(std::is_trivially_copyable<value_type>::value,
                "easy::snapshot requires trivially copyable objects");
  assert(value != nullptr && "easy::snapshot does not accept null pointers");
  return make_snapshot_value(value, std::forward<Bindings>(bindings)...);
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
