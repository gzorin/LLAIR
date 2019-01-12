//-*-C++-*-
#ifndef LLAIR_IR_ENTRYPOINT
#define LLAIR_IR_ENTRYPOINT

#include <llvm/ADT/ilist_node.h>
#include <llvm/ADT/Twine.h>

#include <string>

namespace llvm {
class Function;
template<typename ValueSubClass> class SymbolTableListTraits;
class Twine;
}

namespace llair {

class Module;

class EntryPoint : public llvm::ilist_node<EntryPoint> {
public:

  enum Type {
    kVertex,
    kFragment,
    kCompute
  };

  static const EntryPoint*  Get(const llvm::Function *);
  static EntryPoint*        Get(llvm::Function *);

  static EntryPoint    *Create(Type, llvm::Function *, Module * = nullptr);

  const Module         *getModule() const { return d_module; }
  Module               *getModule()       { return d_module; }

  Type                  getType() const { return d_type; }
  void                  setType(Type type) { d_type = type; }

  const llvm::Function *getFunction() const { return d_function; }
  llvm::Function       *getFunction()       { return d_function; }

  void                  setFunction(llvm::Function *function) { d_function = function; }

  llvm::StringRef       getName() const;

private:

  EntryPoint(Type, llvm::Function *, Module *);

  void setModule(Module *);

  Module *d_module;

  llvm::Function *d_function;
  Type d_type;

  friend class llvm::SymbolTableListTraits<EntryPoint>;
};

}

#endif
