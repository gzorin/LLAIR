//-*-C++-*-
#ifndef LLAIR_INTERFACESCOPE_H
#define LLAIR_INTERFACESCOPE_H

#include <llair/IR/Interface.h>

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/StringRef.h>

#include <algorithm>
#include <memory>
#include <numeric>
#include <unordered_map>

namespace llvm {
class Function;
class Module;
class SwitchInst;
class Type;
} // End namespace llvm

namespace llair {

class Class;
class Dispatcher;
class Interface;
class LLAIRContext;
class Module;

class InterfaceScope {
public:

    InterfaceScope(llvm::StringRef, LLAIRContext&);
    ~InterfaceScope();

    llair::Module *module() { return d_module.get(); }

    void insertInterface(Interface *);
    void insertClass(const Class *);

private:

    std::unique_ptr<llair::Module> d_module;

    std::unordered_map<const Interface *, Dispatcher *> d_dispatchers;

    std::unordered_map<const Class *, uint32_t> d_klasses;

    struct Hash {
        std::size_t operator()(llvm::StringRef name) const {
            return llvm::hash_value(name);
        }
    };

    std::unordered_multimap<llvm::StringRef, const Interface *, Hash> d_interface_index;
    std::unordered_multimap<llvm::StringRef, const Class *, Hash > d_klass_index;

    friend class Interface;
};

} // End namespace llair

#endif