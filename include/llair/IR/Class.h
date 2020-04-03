//-*-C++-*-
#ifndef LLAIR_IR_CLASS
#define LLAIR_IR_CLASS

#include <llair/IR/Named.h>

#include <llvm/ADT/ArrayRef.h>
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

        llvm::StringRef getName()     const { return d_name;     }
        llvm::Function *getFunction() const { return d_function; }

    private:

        Method(llvm::StringRef, llvm::Function *);

        std::string      d_name;
        llvm::Function  *d_function = nullptr;

        llvm::TypedTrackingMDRef<llvm::MDTuple> d_md;

        friend class Class;
    };

    static Class *Create(llvm::StructType *, llvm::ArrayRef<llvm::StringRef>, llvm::ArrayRef<llvm::Function *>, llvm::StringRef = "", Module * = nullptr);

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

    bool doesImplement(const Interface *) const;

    llvm::MDNode *      metadata() { return d_md.get(); }
    const llvm::MDNode *metadata() const { return d_md.get(); }

    void print(llvm::raw_ostream&) const;

    void dump() const override;

private:

    Class(llvm::StructType *, llvm::ArrayRef<llvm::StringRef>, llvm::ArrayRef<llvm::Function *>, llvm::StringRef, Module *);
    Class(llvm::MDNode *, Module *);

    void setModule(Module *);

    // Named overrides:
    LLAIRContext& getContext() const override;

    llvm::StructType *d_type = nullptr;

    std::size_t d_method_count = 0;
    Method     *d_methods      = nullptr;

    Module *d_module = nullptr;

    llvm::TypedTrackingMDRef<llvm::MDNode>          d_md;

    friend struct module_ilist_traits<Class>;
    friend class Module;
};

} // End namespace llair

#endif