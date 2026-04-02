#ifndef CONTEXT
#define CONTEXT

#include <vector>
#include <memory>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <type_traits>
#include <string>

#include <easy/runtime/Function.h>

namespace easy {

struct serialized_arg {
  std::vector<char> buf;

  serialized_arg(char* serialized) {
    uint32_t size = *reinterpret_cast<uint32_t const*>(serialized);
    const char* data = serialized + sizeof(uint32_t);
    buf.insert(buf.end(), data, data+size);

    free(serialized);
  }

  // Direct construction from raw memory (bypasses LLVM-generated serialization)
  serialized_arg(const void* data, size_t size) {
    assert((data != nullptr || size == 0) && "serialized_arg received null data with non-zero size");
    buf.resize(size);
    if (size != 0)
      std::memcpy(buf.data(), data, size);
  }
};

template<class T>
struct serialized_array {
  std::vector<char> buf;
  size_t count;

  serialized_array(const T* data, size_t n) : count(n) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "easy::snapshot_array requires trivially copyable elements");
    assert((data != nullptr || n == 0) && "serialized_array received null data with non-zero count");
    buf.resize(sizeof(T) * n);
    if (n != 0)
      std::memcpy(buf.data(), data, sizeof(T) * n);
  }
};

typedef void* layout_id;

struct ArgumentBase {

  enum ArgumentKind {
    AK_Forward,
    AK_Int,
    AK_Float,
    AK_Ptr,
    AK_Struct,
    AK_Array,
    AK_Module,
  };

  ArgumentBase() = default;
  virtual ~ArgumentBase() = default;

  bool operator==(ArgumentBase const &Other) const {
    return this->kind() == Other.kind() &&
        this->compareWithSameType(Other);
  }

  template<class ArgTy>
  std::enable_if_t<std::is_base_of<ArgumentBase, ArgTy>::value, ArgTy const*>
  as() const {
    if(kind() == ArgTy::Kind) return static_cast<ArgTy const*>(this);
    else return nullptr;
  }

  template<class ArgTy>
  std::enable_if_t<std::is_base_of<ArgumentBase, ArgTy>::value, ArgTy*>
  as() {
    if(kind() == ArgTy::Kind) return static_cast<ArgTy*>(this);
    else return nullptr;
  }

  friend std::hash<easy::ArgumentBase>;

  virtual ArgumentKind kind() const noexcept = 0;

  protected:
  virtual bool compareWithSameType(ArgumentBase const&) const = 0;
  virtual size_t hash() const noexcept = 0;
};

#define DeclareArgument(Name, Type) \
  class Name##Argument \
    : public ArgumentBase { \
    \
    using HashType = std::remove_const_t<std::remove_reference_t<Type>>; \
    \
    Type Data_; \
    public: \
    Name##Argument(Type D) : ArgumentBase(), Data_(D) {} \
    virtual ~Name ## Argument() override = default ;\
    Type get() const { return Data_; } \
    static constexpr ArgumentKind Kind = AK_##Name;\
    ArgumentKind kind() const noexcept override  { return Kind; } \
    \
    protected: \
    bool compareWithSameType(ArgumentBase const& Other) const override { \
      auto const &OtherCast = static_cast<Name##Argument const&>(Other); \
      return Data_ == OtherCast.Data_; \
    } \
    \
    size_t hash() const noexcept override  { return std::hash<HashType>{}(Data_); } \
  }

DeclareArgument(Forward, unsigned);
DeclareArgument(Int, int64_t);
DeclareArgument(Float, double);
DeclareArgument(Ptr, void const*);
DeclareArgument(Module, easy::Function const&);

class StructArgument
    : public ArgumentBase {
 public:
  struct ArrayBinding {
    size_t Offset_;
    std::vector<char> Data_;
    size_t Count_;
    size_t ElementSize_;

    bool operator==(ArrayBinding const& Other) const {
      return Offset_ == Other.Offset_ &&
             Count_ == Other.Count_ &&
             ElementSize_ == Other.ElementSize_ &&
             Data_ == Other.Data_;
    }
  };

 private:
  serialized_arg Data_;
  std::vector<ArrayBinding> ArrayBindings_;

  public:
  StructArgument(serialized_arg &&arg, std::vector<ArrayBinding> bindings = {})
    : ArgumentBase(), Data_(std::move(arg)), ArrayBindings_(std::move(bindings)) {}
  virtual ~StructArgument() override = default;
  std::vector<char> const & get() const { return Data_.buf; }
  std::vector<ArrayBinding> const& getArrayBindings() const { return ArrayBindings_; }
  void addArrayBinding(ArrayBinding binding) { ArrayBindings_.push_back(std::move(binding)); }
  static constexpr ArgumentKind Kind = AK_Struct;
  ArgumentKind kind() const noexcept override  { return Kind; }

  protected:
  bool compareWithSameType(ArgumentBase const& Other) const override {
    auto const &OtherCast = static_cast<StructArgument const&>(Other);
    return get() == OtherCast.get() &&
           getArrayBindings() == OtherCast.getArrayBindings();
  }

  size_t hash() const noexcept override {
    std::hash<int64_t> hash{};
    size_t R = 0;
    for (char c : get())
      R ^= hash(c);
    for (auto const &Binding : getArrayBindings()) {
      R ^= hash(static_cast<int64_t>(Binding.Offset_));
      R ^= hash(static_cast<int64_t>(Binding.Count_));
      R ^= hash(static_cast<int64_t>(Binding.ElementSize_));
      for (char c : Binding.Data_)
        R ^= hash(c);
    }
    return R;
  }
};

class ArrayArgument
    : public ArgumentBase {
  std::vector<char> Data_;
  size_t Count_;
  size_t ElementSize_;

  public:
  ArrayArgument(std::vector<char> data, size_t count, size_t elementSize)
    : ArgumentBase(), Data_(std::move(data)), Count_(count), ElementSize_(elementSize) {}
  virtual ~ArrayArgument() override = default;
  std::vector<char> const & get() const { return Data_; }
  size_t getCount() const { return Count_; }
  size_t getElementSize() const { return ElementSize_; }
  static constexpr ArgumentKind Kind = AK_Array;
  ArgumentKind kind() const noexcept override  { return Kind; }

  protected:
  bool compareWithSameType(ArgumentBase const& Other) const override {
    auto const &OtherCast = static_cast<ArrayArgument const&>(Other);
    return Count_ == OtherCast.Count_ && ElementSize_ == OtherCast.ElementSize_ && Data_ == OtherCast.Data_;
  }

  size_t hash() const noexcept override {
    std::hash<int64_t> hash{};
    size_t R = hash(static_cast<int64_t>(Count_)) ^ hash(static_cast<int64_t>(ElementSize_));
    for (char c : Data_)
      R ^= hash(c);
    return R;
  }
};

// class that holds information about the just-in-time context
class Context {

  std::vector<std::unique_ptr<ArgumentBase>> ArgumentMapping_;
  unsigned OptLevel_ = 2, OptSize_ = 0;
  std::string DebugFile_;

  // describes how the arguments of the function are passed
  //  struct arguments can be packed in a single int, or passed field by field,
  //  keep track of how many arguments a parameter takes
  std::vector<layout_id> ArgumentLayout_;

  template<class ArgTy, class ... Args>
  inline Context& setArg(Args && ... args) {
    ArgumentMapping_.emplace_back(new ArgTy(std::forward<Args>(args)...));
    return *this;
  }

  public:

  Context() = default;

  bool operator==(const Context&) const;
  
  // set the mapping between
  Context& setParameterIndex(unsigned);
  Context& setParameterInt(int64_t);
  Context& setParameterFloat(double);
  Context& setParameterPointer(void const*);
  Context& setParameterStruct(serialized_arg, std::vector<StructArgument::ArrayBinding> Bindings = {});
  Context& setParameterArray(std::vector<char>, size_t Count, size_t ElementSize);
  Context& setParameterModule(easy::Function const&);
  Context& bindArrayToLastStruct(size_t Offset, std::vector<char> Data, size_t Count, size_t ElementSize);

  Context& setArgumentLayout(layout_id id) {
    ArgumentLayout_.push_back(id); // each layout id is associated with a number of fields in the bitcode tracker
    return *this;
  }

  decltype(ArgumentLayout_) const & getLayout() const {
    return ArgumentLayout_;
  }

  template<class T>
  Context& setParameterTypedPointer(T* ptr) {
    return setParameterPointer(reinterpret_cast<const void*>(ptr));
  }

  Context& setOptLevel(unsigned OptLevel, unsigned OptSize) {
    OptLevel_ = OptLevel;
    OptSize_ = OptSize;
    return *this;
  }

  Context& setDebugFile(std::string const &File) {
    DebugFile_ = File;
    return *this;
  }

  std::pair<unsigned, unsigned> getOptLevel() const {
    return std::make_pair(OptLevel_, OptSize_);
  }

  std::string const& getDebugFile() const {
    return DebugFile_;
  }

  auto begin() const { return ArgumentMapping_.begin(); }
  auto end() const { return ArgumentMapping_.end(); }
  size_t size() const { return ArgumentMapping_.size(); }

  ArgumentBase const& getArgumentMapping(size_t i) const {
    return *ArgumentMapping_[i];
  }

  friend bool operator<(easy::Context const &C1, easy::Context const &C2);
}; 

}

namespace std
{
  template<class L, class R> struct hash<std::pair<L, R>>
  {
    typedef std::pair<L,R> argument_type;
    typedef std::size_t result_type;
    result_type operator()(argument_type const& s) const noexcept {
      return std::hash<L>{}(s.first) ^ std::hash<R>{}(s.second);
    }
  };

  template<> struct hash<easy::ArgumentBase>
  {
    typedef easy::ArgumentBase argument_type;
    typedef std::size_t result_type;
    result_type operator()(argument_type const& s) const noexcept {
      return s.hash();
    }
  };

  template<> struct hash<easy::Context>
  {
    typedef easy::Context argument_type;
    typedef std::size_t result_type;
    result_type operator()(argument_type const& C) const noexcept {
      size_t H = 0;
      std::hash<easy::ArgumentBase> ArgHash;
      std::hash<std::pair<unsigned, unsigned>> OptHash;
      for(auto const &Arg : C)
        H ^= ArgHash(*Arg);
      H ^= OptHash(C.getOptLevel());
      return H;
    }
  };
}


#endif
