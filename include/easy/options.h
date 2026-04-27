#ifndef OPTIONS
#define OPTIONS

#include <easy/runtime/Context.h>
#include <easy/snapshot.h>
#include <cassert>
#include <cstring>
#include <memory>
#include <type_traits>

#define EASY_NEW_OPTION_STRUCT(Name) \
  struct Name; \
  template<> struct is_option<Name> { \
    static constexpr bool value = true; }; \
  struct Name

#define EASY_HANDLE_OPTION_STRUCT(Name, Ctx) \
  void handle(easy::Context &Ctx) const


namespace easy {
namespace options{

  template<class T>
  struct is_option {
    static constexpr bool value = false;
  };

  EASY_NEW_OPTION_STRUCT(opt_level)
    : public std::pair<unsigned, unsigned> {

    opt_level(unsigned OptLevel, unsigned OptSize)
               : std::pair<unsigned, unsigned>(OptLevel,OptSize) {}

    EASY_HANDLE_OPTION_STRUCT(opt_level, C) {
      C.setOptLevel(first, second);
    }
  };

  // option used for writing the optimized IR to a file and
  // the pre/post-optimize IR to sibling "*.before.*" / "*.after.*" files.
  EASY_NEW_OPTION_STRUCT(dump_ir) {
    dump_ir(std::string const &file)
               : file_(file) {}

    EASY_HANDLE_OPTION_STRUCT(dump_ir, C) {
      C.setDebugFile(file_);
    }

    private:
    std::string file_;
  };

  EASY_NEW_OPTION_STRUCT(recursive_jit) {
    explicit recursive_jit(bool Enabled = true)
        : Enabled_(Enabled) {}

    EASY_HANDLE_OPTION_STRUCT(recursive_jit, C) {
      C.setRecursiveJit(Enabled_);
    }

   private:
    bool Enabled_;
  };

  template<class T>
  struct global_snapshot_option;

  template<class T>
  struct is_option<global_snapshot_option<T>> {
    static constexpr bool value = true;
  };

  template<class T>
  struct global_snapshot_option {
    using value_type = std::remove_cv_t<T>;

    explicit global_snapshot_option(value_type const* Ptr) : Ptr_(Ptr) {}

    void handle(easy::Context &C) const {
      static_assert(std::is_trivially_copyable<value_type>::value,
                    "easy::options::global_snapshot requires a trivially copyable type");
      assert(Ptr_ != nullptr && "easy::options::global_snapshot does not accept null");
      C.setGlobalStruct(reinterpret_cast<void const*>(Ptr_),
                        easy::serialized_arg(Ptr_, sizeof(value_type)));
    }

   private:
    value_type const* Ptr_;
  };

  template<class T>
  global_snapshot_option<std::remove_reference_t<T>>
  global_snapshot(T const& Value) {
    using value_type = std::remove_reference_t<T>;
    return global_snapshot_option<value_type>(std::addressof(Value));
  }

  template<class T>
  global_snapshot_option<std::remove_cv_t<T>>
  global_snapshot(T const* Value) {
    using value_type = std::remove_cv_t<T>;
    return global_snapshot_option<value_type>(Value);
  }

  template<class T>
  struct global_partial_snapshot_option;

  template<class T>
  struct is_option<global_partial_snapshot_option<T>> {
    static constexpr bool value = true;
  };

  template<class T>
  struct global_partial_snapshot_option {
    using value_type = std::remove_cv_t<T>;

    global_partial_snapshot_option(value_type const* Ptr,
                                   std::vector<easy::StructFieldBinding> FieldBindings,
                                   std::vector<easy::StructArrayBinding> ArrayBindings)
        : Ptr_(Ptr),
          FieldBindings_(std::move(FieldBindings)),
          ArrayBindings_(std::move(ArrayBindings)) {}

    void handle(easy::Context &C) const {
      static_assert(std::is_trivially_copyable<value_type>::value,
                    "easy::options::global_partial_snapshot requires a trivially copyable type");
      assert(Ptr_ != nullptr && "easy::options::global_partial_snapshot does not accept null");
      C.setGlobalPartialStruct(reinterpret_cast<void const*>(Ptr_),
                               FieldBindings_,
                               ArrayBindings_);
    }

   private:
    value_type const* Ptr_;
    std::vector<easy::StructFieldBinding> FieldBindings_;
    std::vector<easy::StructArrayBinding> ArrayBindings_;
  };

  template<class Object, class Field>
  void append_global_partial_binding(Object const* Ptr,
                                     std::vector<easy::StructFieldBinding>& FieldBindings,
                                     std::vector<easy::StructArrayBinding>&,
                                     easy::snapshot_field_binding_builder<Object, Field> Binding) {
    auto SnapshotBinding = easy::make_snapshot_field_binding<Object>(Ptr, Binding);
    std::vector<char> Data(SnapshotBinding.size);
    if (!Data.empty())
      std::memcpy(Data.data(), SnapshotBinding.data, SnapshotBinding.size);
    FieldBindings.push_back(
        easy::StructFieldBinding{SnapshotBinding.offset, std::move(Data)});
  }

  template<class Object, class Pointer>
  void append_global_partial_binding(Object const* Ptr,
                                     std::vector<easy::StructFieldBinding>&,
                                     std::vector<easy::StructArrayBinding>& ArrayBindings,
                                     easy::snapshot_array_binding_builder<Object, Pointer> Binding) {
    auto SnapshotBinding = easy::make_snapshot_array_binding<Object>(Ptr, Binding);
    std::vector<char> Data(SnapshotBinding.count * SnapshotBinding.element_size);
    if (!Data.empty())
      std::memcpy(Data.data(), SnapshotBinding.data, Data.size());
    ArrayBindings.push_back(easy::StructArrayBinding{
        SnapshotBinding.offset,
        std::move(Data),
        SnapshotBinding.count,
        SnapshotBinding.element_size});
  }

  template<class T, class ... Bindings>
  global_partial_snapshot_option<std::remove_reference_t<T>>
  global_partial_snapshot(T const& Value, Bindings&&... BindingsToUse) {
    using value_type = std::remove_reference_t<T>;
    static_assert(std::is_class<value_type>::value,
                  "easy::options::global_partial_snapshot requires a struct/class object");
    static_assert(std::is_trivially_copyable<value_type>::value,
                  "easy::options::global_partial_snapshot requires a trivially copyable object");
    std::vector<easy::StructFieldBinding> FieldBindings;
    std::vector<easy::StructArrayBinding> ArrayBindings;
    FieldBindings.reserve(sizeof...(Bindings));
    ArrayBindings.reserve(sizeof...(Bindings));
    using expand = int[];
    (void)expand{0, (append_global_partial_binding<value_type>(
                        std::addressof(Value),
                        FieldBindings,
                        ArrayBindings,
                        std::forward<Bindings>(BindingsToUse)),
                    0)...};
    return global_partial_snapshot_option<value_type>(std::addressof(Value),
                                                      std::move(FieldBindings),
                                                      std::move(ArrayBindings));
  }

  template<class T, class ... Bindings>
  global_partial_snapshot_option<std::remove_cv_t<T>>
  global_partial_snapshot(T const* Value, Bindings&&... BindingsToUse) {
    assert(Value != nullptr && "easy::options::global_partial_snapshot does not accept null");
    return global_partial_snapshot(*Value, std::forward<Bindings>(BindingsToUse)...);
  }
}
}

#endif // OPTIONS
