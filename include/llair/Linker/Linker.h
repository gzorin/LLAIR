//-*-C++-*-
#ifndef LLAIR_LINKER
#define LLAIR_LINKER

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>

namespace llvm {
class Function;
class Module;
class SwitchInst;
class Type;
} // End namespace llvm

namespace llair {

class Class;
class Interface;
class LLAIRContext;
class Module;

void linkModules(Module *, const Module *);
void finalizeInterfaces(Module *, llvm::ArrayRef<Interface *>, std::function<uint32_t(const Class*)>);

class Linker {
public:

    class Delegate {
        virtual ~Delegate();

        virtual uint32_t getKindForClass(const Class *) = 0;
    };

    Linker(Delegate&, llvm::StringRef, LLAIRContext &);

    void addInterface(Interface *);

    void linkModule(const Module *);

    std::unique_ptr<Module> releaseModule();

private:

    Delegate& d_delegate;

    std::unique_ptr<Module> d_module;

    llvm::StringMap<llvm::DenseSet<Interface *>> d_interface_index;
};

} // End namespace llair

#endif
