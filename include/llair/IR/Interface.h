//-*-C++-*-
#ifndef LLAIR_INTERFACE
#define LLAIR_INTERFACE

#include <llair/IR/LLAIRContext.h>

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

class LLAIRContext;

class Interface {
public:

    class Method {
    public:

        llvm::StringRef     getName()          const { return d_name; }
        llvm::StringRef     getQualifiedName() const { return d_qualifiedName; }
        llvm::FunctionType *getType()          const { return d_type; };

    private:

        Method(llvm::StringRef, llvm::StringRef, llvm::FunctionType *);

        std::string         d_name;
        std::string         d_qualifiedName;
        llvm::FunctionType *d_type = nullptr;

        friend class Interface;
    };

    static Interface *get(LLAIRContext&, llvm::StructType *, llvm::ArrayRef<llvm::StringRef>, llvm::ArrayRef<llvm::StringRef>, llvm::ArrayRef<llvm::FunctionType *>);

    ~Interface();

    LLAIRContext& getContext() const { return d_context; }

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

    void dump() const;

private:

    Interface(LLAIRContext&, llvm::StructType *, llvm::ArrayRef<llvm::StringRef>, llvm::ArrayRef<llvm::StringRef>, llvm::ArrayRef<llvm::FunctionType *>);

    LLAIRContext& d_context;

    llvm::StructType *d_type = nullptr;

    std::size_t d_method_count = 0;
    Method *    d_methods      = nullptr;
};

} // End namespace llair

#endif
