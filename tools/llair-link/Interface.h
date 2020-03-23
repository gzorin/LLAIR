//-*-C++-*-
#ifndef LLAIR_INTERFACE
#define LLAIR_INTERFACE

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/StringRef.h>

#include <string>

namespace llvm {
class FunctionType;
class StructType;
class raw_ostream;
} // End namespace llvm

namespace llair {

class InterfaceScope;

class Interface {
public:

    struct Method {
        std::string         name;
        std::string         qualifiedName;
        llvm::FunctionType *type = nullptr;
    };

    static Interface *get(InterfaceScope&, llvm::StructType *, llvm::ArrayRef<Method>);

    ~Interface();

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

    Interface(InterfaceScope&, llvm::StructType *, llvm::ArrayRef<Method>);

    InterfaceScope& d_scope;

    llvm::StructType *d_type = nullptr;

    std::size_t d_method_count = 0;
    Method *    d_methods      = nullptr;
};

inline llvm::hash_code hash_value(const llair::Interface::Method& method) {
    return llvm::hash_combine(method.name, method.qualifiedName, method.type);
}

} // End namespace llair

#endif
