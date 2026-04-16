#include "easy/runtime/Context.h"
#include <stdexcept>

using namespace easy;

Context& Context::setParameterIndex(unsigned param_idx) {
  return setArg<ForwardArgument>(param_idx);
}

Context& Context::setParameterInt(int64_t val) {
  return setArg<IntArgument>(val);
}

Context& Context::setParameterFloat(double val) {
  return setArg<FloatArgument>(val);
}

Context& Context::setParameterPointer(const void* val) {
  return setArg<PtrArgument>(val);
}

Context& Context::setParameterStruct(serialized_arg arg,
                                     std::vector<StructArrayBinding> Bindings) {
  return setArg<StructArgument>(std::move(arg), std::move(Bindings));
}

Context& Context::setPartialStruct(unsigned Index,
                                   std::vector<StructFieldBinding> FieldBindings,
                                   std::vector<StructArrayBinding> ArrayBindings) {
  return setArg<PartialStructArgument>(Index, std::move(FieldBindings), std::move(ArrayBindings));
}

Context& Context::setParameterArray(std::vector<char> data, size_t Count, size_t ElementSize) {
  return setArg<ArrayArgument>(std::move(data), Count, ElementSize);
}

Context& Context::setParameterModule(easy::Function const &F) {
  return setArg<ModuleArgument>(F);
}

Context& Context::bindArrayToLastStruct(size_t Offset,
                                        std::vector<char> Data,
                                        size_t Count,
                                        size_t ElementSize) {
  if (ArgumentMapping_.empty())
    throw std::invalid_argument("bindArrayToLastStruct requires a previous struct parameter");

  if (auto *Struct = ArgumentMapping_.back()->as<StructArgument>()) {
    Struct->addArrayBinding(StructArrayBinding{
        Offset, std::move(Data), Count, ElementSize});
    return *this;
  }

  if (auto *Partial = ArgumentMapping_.back()->as<PartialStructArgument>()) {
    Partial->addArrayBinding(StructArrayBinding{
        Offset, std::move(Data), Count, ElementSize});
    return *this;
  }

  throw std::invalid_argument("bindArrayToLastStruct must follow a snapshot or partial-struct parameter");
}

Context& Context::bindFieldToLastPartialStruct(size_t Offset,
                                               std::vector<char> Data) {
  if (ArgumentMapping_.empty())
    throw std::invalid_argument("bindFieldToLastPartialStruct requires a previous partial-struct parameter");

  auto *Partial = ArgumentMapping_.back()->as<PartialStructArgument>();
  if (!Partial)
    throw std::invalid_argument("bindFieldToLastPartialStruct must follow a partial-struct parameter");

  Partial->addFieldBinding(StructFieldBinding{Offset, std::move(Data)});
  return *this;
}

bool Context::operator==(const Context& Other) const {
  if(getOptLevel() != Other.getOptLevel())
    return false;
  if(size() != Other.size())
    return false;

  for(auto this_it = begin(), other_it = Other.begin();
      this_it != end(); ++this_it, ++other_it) {
    ArgumentBase &ThisArg = **this_it;
    ArgumentBase &OtherArg = **other_it;
    if(!(ThisArg == OtherArg))
      return false;
  }

  return true;
}
