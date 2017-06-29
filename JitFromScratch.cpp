#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include "SimpleOrcJit.h"

llvm::Expected<std::string> codegenIR(Module *module, unsigned items) {
  using namespace llvm;

  LLVMContext &ctx = module->getContext();
  IRBuilder<> Builder(ctx);

  auto name = "integerDistance";
  auto intTy = Type::getInt32Ty(ctx);
  auto intPtrTy = intTy->getPointerTo();
  auto signature = FunctionType::get(intPtrTy, {intPtrTy, intPtrTy}, false);
  auto linkage = Function::ExternalLinkage;

  auto fn = Function::Create(signature, linkage, name, module);
  fn->setName(name); // so the CompileLayer can find it

  Builder.SetInsertPoint(BasicBlock::Create(ctx, "entry", fn));
  {
    auto argIt = fn->arg_begin();
    Argument &argPtrX = *argIt;
    Argument &argPtrY = *(++argIt);

    argPtrX.setName("x");
    argPtrY.setName("y");

    auto allocSig = FunctionType::get(intPtrTy, {intTy}, false);
    Value *allocFunction =
        module->getOrInsertFunction("_Z18customIntAllocatorj", allocSig);

    Value *results_size = ConstantInt::get(intTy, items);
    Value *results_ptr = Builder.CreateCall(allocFunction, {results_size});

    auto absSig = FunctionType::get(intTy, {intTy}, false);
    Value *absFunction = module->getOrInsertFunction("abs", absSig);

    for (unsigned int i = 0; i < items; i++) {
      Value *xi_ptr = Builder.CreateConstInBoundsGEP1_32(intTy, &argPtrX, i);
      Value *yi_ptr = Builder.CreateConstInBoundsGEP1_32(intTy, &argPtrY, i);

      Value *x0 = Builder.CreateLoad(xi_ptr);
      Value *y0 = Builder.CreateLoad(yi_ptr);
      Value *difference = Builder.CreateSub(x0, y0);
      Value *absDifference = Builder.CreateCall(absFunction, {difference});

      Value *ri_ptr = Builder.CreateConstInBoundsGEP1_32(intTy, results_ptr, i);
      Builder.CreateStore(absDifference, ri_ptr);
    }

    Builder.CreateRet(results_ptr);
  }

  std::string error;
  raw_string_ostream es(error);

  if (verifyFunction(*fn, &es))
    return make_error<StringError>(
        Twine("Function verification failed:\n", es.str()), std::error_code());

  if (verifyModule(*module, &es))
    return make_error<StringError>(
        Twine("Module verification failed:\n", es.str()), std::error_code());

  return name;
}

template <typename T, size_t sizeOfArray>
constexpr unsigned arrayElements(T (&)[sizeOfArray]) {
  return sizeOfArray;
}

int *customIntAllocator(unsigned items) {
  static int memory[100];
  static unsigned allocIdx = 0;

  if (allocIdx + items > arrayElements(memory))
    exit(-1);

  int *block = memory + allocIdx;
  allocIdx += items;

  return block;
}

// goal for jit-basics:
// replace this function with a runtime-time compiled version
template <size_t sizeOfArray>
int *integerDistances(const int (&x)[sizeOfArray], int *y,
                      std::function<int *(int *, int *)> jittedFn) {
  return jittedFn(const_cast<int *>(x), y);
}

int main(int argc, char **argv) {
  using namespace llvm;

  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram X(argc, argv);

  atexit(llvm_shutdown);

  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  auto *targetMachine = EngineBuilder().selectTarget();
  auto jit = std::make_unique<SimpleOrcJit>(*targetMachine);

  LLVMContext context;
  auto module = std::make_unique<Module>("JitFromScratch", context);
  module->setDataLayout(targetMachine->createDataLayout());

  int x[]{0, 1, 2};
  int y[]{3, 1, -1};

  Expected<std::string> jittedFnName =
      codegenIR(module.get(), arrayElements(x));

  if (!jittedFnName)
    outs() << toString(jittedFnName.takeError());

  jit->submitModule(std::move(module));
  auto jittedFnPtr = jit->getFunction<int *(int *, int *)>(*jittedFnName);

  int *z = integerDistances(x, y, jittedFnPtr);

  outs() << "Integer Distances: ";
  outs() << z[0] << ", " << z[1] << ", " << z[2] << "\n\n";
  outs().flush();

  return 0;
}
