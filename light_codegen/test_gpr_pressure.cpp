// test_gpr_pressure.cpp — validate the extended GPR scratch pool.
//
// The IR intentionally keeps 17 integer SSA results in one block:
//   9 independent add-with-immediate terms plus 8 reduction adds.
// With eight incoming integer arguments, the old caller-saved-only pool
// had only x8..x15 available and failed with "scratch OOM (binop)".
// Round 8l saves x19..x28 in the frame and makes them available as
// extra scratch registers, so this shape should compile and execute.

#include "light_aarch64.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(__aarch64__) || defined(__arm64__)
#  define LIGHT_PRESSURE_HOST_AARCH64 1
#  include <sys/mman.h>
#else
#  define LIGHT_PRESSURE_HOST_AARCH64 0
#endif

using namespace llvm;

namespace {

std::unique_ptr<Module> NewAArch64Module(LLVMContext &Ctx, const char *Name) {
  auto M = std::make_unique<Module>(Name, Ctx);
  M->setTargetTriple("aarch64-unknown-linux-gnu");
  M->setDataLayout("e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128");
  return M;
}

struct EmitOut {
  ::light::Status status;
  std::vector<uint8_t> code;
  std::string reason;
};

EmitOut EmitFunction(const Function &F) {
  std::vector<uint8_t> buf(8192);
  ::light::Result r = ::light::emit(F, buf.data(), buf.size(), nullptr, 0);
  EmitOut out;
  out.status = r.status;
  out.reason = r.reason;
  if (r.status == ::light::Status::Ok)
    out.code.assign(buf.begin(), buf.begin() + r.codeBytes);
  return out;
}

Function *BuildWideI64(Module &M) {
  LLVMContext &C = M.getContext();
  Type *I64 = Type::getInt64Ty(C);
  std::vector<Type *> args(8, I64);
  FunctionType *FT = FunctionType::get(I64, args, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "wide_i64_pressure", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);

  std::vector<Value *> terms;
  terms.reserve(9);
  for (unsigned i = 0; i < 9; ++i) {
    Value *base = F->getArg(i % 8);
    terms.push_back(B.CreateAdd(base, ConstantInt::get(I64, 101 + i),
                                "term" + std::to_string(i)));
  }

  Value *sum = terms[0];
  for (unsigned i = 1; i < terms.size(); ++i)
    sum = B.CreateAdd(sum, terms[i], "sum" + std::to_string(i));
  B.CreateRet(sum);
  return F;
}

#if LIGHT_PRESSURE_HOST_AARCH64
void *MakeExecutable(const std::vector<uint8_t> &code) {
  void *page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) return nullptr;
  std::memcpy(page, code.data(), code.size());
  __builtin___clear_cache((char *)page, (char *)page + code.size());
  return page;
}
#endif

} // namespace

int main() {
  LLVMContext Ctx;
  auto M = NewAArch64Module(Ctx, "light_gpr_pressure");
  Function *F = BuildWideI64(*M);
  EmitOut eo = EmitFunction(*F);
  if (eo.status != ::light::Status::Ok) {
    std::printf("FAIL: emit status=%d reason=%s\n",
                (int)eo.status, eo.reason.c_str());
    return 1;
  }
  std::printf("  emit wide_i64_pressure OK (%zu bytes)\n", eo.code.size());

#if LIGHT_PRESSURE_HOST_AARCH64
  using Fn = long long (*)(long long, long long, long long, long long,
                           long long, long long, long long, long long);
  auto fn = (Fn)MakeExecutable(eo.code);
  if (!fn) {
    std::printf("FAIL: mmap/exec setup\n");
    return 1;
  }

  struct Case {
    long long a[8];
  };
  Case cases[] = {
      {{1, 2, 3, 4, 5, 6, 7, 8}},
      {{-10, 20, -30, 40, -50, 60, -70, 80}},
      {{1000, 0, 0, 0, 0, 0, 0, -1000}},
  };

  int failures = 0;
  for (const Case &tc : cases) {
    long long got = fn(tc.a[0], tc.a[1], tc.a[2], tc.a[3],
                       tc.a[4], tc.a[5], tc.a[6], tc.a[7]);
    long long want = 0;
    for (unsigned i = 0; i < 9; ++i)
      want += tc.a[i % 8] + (long long)(101 + i);
    bool ok = (got == want);
    std::printf("  wide_i64_pressure(...) = %lld want %lld %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }
  if (failures) {
    std::printf("FAILED: %d case(s)\n", failures);
    return 1;
  }
  std::printf("PASS: light GPR pressure execution\n");
#else
  std::printf("PASS (emit-only): non-AArch64 host, execution skipped\n");
#endif
  return 0;
}
