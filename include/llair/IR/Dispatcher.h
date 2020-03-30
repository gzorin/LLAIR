//-*-C++-*-
#ifndef LLAIR_IR_DISPATCHER_H
#define LLAIR_IR_DISPATCHER_H

#include <llair/IR/Interface.h>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/ilist_node.h>

#include <string>

namespace llvm {
class Function;
class StructType;
class SwitchInst;
class raw_ostream;
} // End namespace llvm

namespace llair {

class Class;
class Module;

template<typename T> struct module_ilist_traits;

class Dispatcher : public llvm::ilist_node<Dispatcher> {
public:
    class Method {
    public:

        ~Method();

        llvm::StringRef getName()     const { return d_interface_method->getName();     }
        llvm::Function *getFunction() const { return d_function; }

    private:

        Method(const Interface *, const Interface::Method *, Module *);

        const Interface::Method *d_interface_method = nullptr;
        llvm::Function          *d_function = nullptr;
        llvm::SwitchInst        *d_switcher = nullptr;

        friend class Dispatcher;
    };

    static Dispatcher *create(const Interface *, Module * = nullptr);

    ~Dispatcher();

    const Interface *getInterface() const { return d_interface; }

    using method_iterator       = Method *;
    using const_method_iterator = const Method *;

    method_iterator       method_begin() { return d_methods; }
    const method_iterator method_begin() const { return d_methods; }

    method_iterator       method_end() { return d_methods + method_size(); }
    const method_iterator method_end() const { return d_methods + method_size(); }

    std::size_t method_size() const { return d_method_count; }

    const Method *findMethod(llvm::StringRef) const;

    void insertImplementation(uint32_t, const Class *);
    void removeImplementation(uint32_t);

    void print(llvm::raw_ostream&) const;

    void dump() const;

private:

    Dispatcher(const Interface *, Module *);

    void setModule(Module *);

    const Interface *d_interface = nullptr;

    std::size_t d_method_count = 0;
    Method     *d_methods      = nullptr;

    Module *d_module = nullptr;

    struct Implementation {
        std::string name;
    };

    llvm::DenseMap<uint32_t, Implementation> d_implementations;

    friend struct module_ilist_traits<Dispatcher>;
};

} // End namespace llair

#endif