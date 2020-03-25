//-*-C++-*-
#ifndef LLAIR_IR_CLASS
#define LLAIR_IR_CLASS

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/ilist_node.h>

#include <string>

namespace llvm {
class Function;
class StructType;
class raw_ostream;
template <typename ValueSubClass> class SymbolTableListTraits;
} // End namespace llvm

namespace llair {

class Module;

class Class : public llvm::ilist_node<Class> {
public:
    struct Method {
        std::string      name;
        llvm::Function  *function = nullptr;
    };

    static Class *create(llvm::StructType *, llvm::ArrayRef<Method>, Module * = nullptr);

    ~Class();

    llvm::StructType *getType() const { return d_type; }

    using method_iterator       = Method *;
    using const_method_iterator = const Method *;

    method_iterator       method_begin() { return d_methods; }
    const method_iterator method_begin() const { return d_methods; }

    method_iterator       method_end() { return d_methods + method_size(); }
    const method_iterator method_end() const { return d_methods + method_size(); }

    std::size_t method_size() const { return d_method_count; }

    const Method *findMethod(llvm::StringRef) const;

    void print(llvm::raw_ostream&) const;

private:

    Class(llvm::StructType *, llvm::ArrayRef<Method>, Module *);

    void setModule(Module *);

    llvm::StructType *d_type = nullptr;

    std::size_t d_method_count = 0;
    Method     *d_methods      = nullptr;

    Module *d_module = nullptr;

    friend class llvm::SymbolTableListTraits<Class>;
};

} // End namespace llair

#endif