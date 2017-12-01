//-*-C++-*-
#ifndef LLAIR_IR_MODULE_H
#define LLAIR_IR_MODULE_H

#include <llair/IR/LLAIRContext.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Module.h>

#include <memory>

namespace llvm {
class LLVMContext;
class Module;
} // End namespace llvm

namespace llair {

class Module
{
public:

  static const Module*          Get(const llvm::Module *);
  static Module*                Get(llvm::Module *);

  Module(llvm::StringRef name, LLAIRContext& context);
  Module(std::unique_ptr<llvm::Module>&& module);
  ~Module();

  const LLAIRContext&         getContext() const { return d_context; }
  LLAIRContext&               getContext()       { return d_context; }

  const llvm::LLVMContext&      getLLContext() const { return d_context.getLLContext(); }
  llvm::LLVMContext&            getLLContext()       { return d_context.getLLContext(); }

  const llvm::Module           *getLLModule() const { return d_llmodule.get(); }
  llvm::Module                 *getLLModule()       { return d_llmodule.get(); }

  void readMetadata();
  void writeMetadata() const;

private:

  LLAIRContext& d_context;
  std::unique_ptr<llvm::Module> d_llmodule;
};

} // End llair namespace

#endif
