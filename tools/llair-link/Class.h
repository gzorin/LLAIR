//-*-C++-*-
#ifndef LLAIR_CLASS_H
#define LLAIR_CLASS_H

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>

#include <string>

namespace llvm {
class Function;
class StructType;
class raw_ostream;
} // End namespace llvm

namespace llair {

class Class {
public:
    struct Method {
        std::string      name;
        llvm::Function  *function = nullptr;
    };

    static Class *create(llvm::StructType *, llvm::ArrayRef<Method>);

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

    Class(llvm::StructType *, llvm::ArrayRef<Method>);

    llvm::StructType *d_type = nullptr;

    std::size_t d_method_count = 0;
    Method *    d_methods      = nullptr;
};

} // End namespace llair

#endif