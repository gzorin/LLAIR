//-*-C++-*-
#ifndef LLAIR_INTERFACESCOPE_H
#define LLAIR_INTERFACESCOPE_H

#include <llvm/ADT/StringRef.h>

#include <memory>
#include <unordered_map>

namespace llvm {
class Function;
class LLVMContext;
class Module;
class SwitchInst;
} // End namespace llvm

namespace llair {

class Class;
class Interface;

class InterfaceScope {
public:

    InterfaceScope(llvm::StringRef, llvm::LLVMContext&);

    llvm::Module *module() { return d_module.get(); }

    void insertInterface(const Interface *);
    void insertClass(const Class *);

    class Implementations {
    public:

        struct Method {
            llvm::Function  *function = nullptr;

        private:

            Method(llvm::Function *function, llvm::SwitchInst *switcher)
            : function(function)
            , switcher(switcher) {
            }

            llvm::SwitchInst *switcher = nullptr;

            friend class Implementations;
        };

        ~Implementations();

        const Interface *interface() const { return d_interface; }

        using method_iterator       = Method *;
        using const_method_iterator = const Method *;

        method_iterator       method_begin() { return d_methods; }
        const method_iterator method_begin() const { return d_methods; }

        method_iterator       method_end() { return d_methods + method_size(); }
        const method_iterator method_end() const { return d_methods + method_size(); }

        std::size_t method_size() const { return d_interface->method_size(); }

    private:

        static std::unique_ptr<Implementations> create(const Interface *, llvm::Module *);

        Implementations(const Interface *, llvm::Module *);

        void addImplementation(const Class *, uint32_t, llvm::StructType *, llvm::ArrayRef<llvm::Function *>, llvm::Module *);

        const Interface *d_interface = nullptr;

        Method *d_methods = nullptr;

        friend class InterfaceScope;
    };

private:

    std::unique_ptr<llvm::Module> d_module;

    std::unordered_map<const Interface *, std::unique_ptr<Implementations>> d_interfaces;

    struct ClassData {
        uint32_t id = 0;
        llvm::StructType *type = nullptr;
        std::vector<llvm::Function *> method_functions;
    };

    std::unordered_map<const Class *, ClassData> d_klasses;

    struct Hash {
        std::size_t operator()(llvm::StringRef name) const {
            return llvm::hash_value(name);
        }
    };

    std::unordered_multimap<llvm::StringRef, const Interface *, Hash> d_interface_index;
    std::unordered_multimap<llvm::StringRef, const Class *, Hash > d_klass_index;
};

} // End namespace llair

#endif