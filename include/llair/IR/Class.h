//-*-C++-*-
#ifndef LLAIR_IR_CLASS
#define LLAIR_IR_CLASS

#include <llair/IR/Named.h>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/ilist_node.h>
#include <llvm/IR/TrackingMDRef.h>

#include <string>

namespace llvm {
class Function;
class StructType;
class raw_ostream;
} // End namespace llvm

namespace llair {

class Interface;
class Module;

template<typename T> struct module_ilist_traits;

class Class : public llvm::ilist_node<Class>,
              public Named {
public:
    class Method {
    public:

        llvm::StringRef getName()     const;
        llvm::Function *getFunction() const;

    private:

        Method(llvm::StringRef, llvm::Function *);
        Method(llvm::Metadata *);

        llvm::TypedTrackingMDRef<llvm::MDTuple> d_md;

        friend class Class;
    };

    static Class *Create(llvm::StructType *, llvm::ArrayRef<llvm::StringRef>, llvm::ArrayRef<llvm::Function *>, llvm::StringRef = "", Module * = nullptr);

    ~Class();

    llvm::StructType *getType() const { return d_type; }
    llvm::Optional<std::size_t> getSize() const { return d_size; }

    llvm::StructType *getTypeWithKind() const { return d_type_with_kind; }
    llvm::Optional<std::size_t> getSizeWithKind() const { return d_size_with_kind; }
    llvm::Optional<std::size_t> getOffsetPastKind() const { return d_offset_past_kind; }

    using method_iterator       = Method *;
    using const_method_iterator = const Method *;

    method_iterator       method_begin() { return d_methods; }
    const method_iterator method_begin() const { return d_methods; }

    method_iterator       method_end() { return d_methods + method_size(); }
    const method_iterator method_end() const { return d_methods + method_size(); }

    std::size_t method_size() const { return d_method_count; }

    const Method *findMethod(llvm::StringRef) const;

    bool doesImplement(const Interface *) const;

    llvm::Metadata *      metadata() { return d_md.get(); }
    const llvm::Metadata *metadata() const { return d_md.get(); }

    void print(llvm::raw_ostream&) const;

    void dump() const override;

private:

    static Class *Create(llvm::Metadata *, Module * = nullptr);

    Class(llvm::StructType *, llvm::ArrayRef<llvm::StringRef>, llvm::ArrayRef<llvm::Function *>, llvm::StringRef, Module *);
    Class(llvm::Metadata *, Module *);

    void setModule(Module *);
    void updateLayout();

    // Named overrides:
    LLAIRContext& getContext() const override;

    llvm::StructType *d_type = nullptr, *d_type_with_kind = nullptr;

    std::size_t d_method_count = 0;
    Method     *d_methods      = nullptr;

    Module *d_module = nullptr;

    llvm::Optional<std::size_t> d_size, d_size_with_kind, d_offset_past_kind;

    llvm::TypedTrackingMDRef<llvm::MDTuple> d_md;

    friend struct module_ilist_traits<Class>;
    friend class Module;
};

} // End namespace llair

#endif