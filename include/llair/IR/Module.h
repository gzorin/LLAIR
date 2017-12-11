//-*-C++-*-
#ifndef LLAIR_IR_MODULE_H
#define LLAIR_IR_MODULE_H

#include <llair/IR/LLAIRContext.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/Twine.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <tuple>

namespace llvm {
class LLVMContext;
class Module;
class ValueSymbolTable;
} // End namespace llvm

namespace llair {
class EntryPoint;
class Module;
} // End namespace llair

namespace llvm {
template<> struct SymbolTableListParentType<llair::EntryPoint> { using type = llair::Module; };
} // End namespace llvm

namespace llair {

class Module
{
public:

  using EntryPointListType = llvm::SymbolTableList<EntryPoint>;

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

  const llvm::ValueSymbolTable& getValueSymbolTable() const { return d_llmodule->getValueSymbolTable(); }
  llvm::ValueSymbolTable&       getValueSymbolTable()       { return d_llmodule->getValueSymbolTable(); }

  //
  struct Version {
    int major = 0, minor = 0, patch = 0;
  };

  //
  void setVersion(const Version& version) { d_version = version; }
  const Version& getVersion() const       { return d_version;    }

  struct Language {
    std::string name;
    Version version;
  };

  void setLanguage(const Language& language) { d_language = language; }
  const Language& getLanguage() const        { return d_language;     }

  //
  const EntryPointListType&     getEntryPointList() const { return d_entry_points; };
  EntryPointListType&           getEntryPointList()       { return d_entry_points; };

  static EntryPointListType Module::*getSublistAccess(EntryPoint *) {
    return &Module::d_entry_points;
  }

  //
  void readMetadata();
  void writeMetadata();

private:

  LLAIRContext& d_context;
  std::unique_ptr<llvm::Module> d_llmodule;

  Version d_version;
  Language d_language;
  EntryPointListType d_entry_points;
};

} // End llair namespace

#endif
