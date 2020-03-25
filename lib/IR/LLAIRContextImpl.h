//-*-C++-*-
#ifndef LLAIRCONTEXTIMPL_H
#define LLAIRCONTEXTIMPL_H

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/StringRef.h>
#include <llair/IR/Interface.h>
#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>

#include <numeric>

namespace llvm {
class Function;
class LLVMContext;
class Module;
} // namespace llvm

namespace llair {

struct InterfaceKeyInfo {
    struct KeyTy {
        llvm::StructType *type;
        llvm::ArrayRef<llvm::StringRef> names;
        llvm::ArrayRef<llvm::StringRef> qualifiedNames;
        llvm::ArrayRef<llvm::FunctionType *> types;

        KeyTy(llvm::StructType *type, llvm::ArrayRef<llvm::StringRef> names, llvm::ArrayRef<llvm::StringRef> qualifiedNames, llvm::ArrayRef<llvm::FunctionType *> types)
        : type(type)
        , names(names)
        , qualifiedNames(qualifiedNames)
        , types(types) {
        }

        bool operator==(const KeyTy& that) const {
            if (type != that.type) {
                return false;
            }

            if (names != that.names) {
                return false;
            }

            if (qualifiedNames != that.qualifiedNames) {
                return false;
            }

            if (types != that.types) {
                return false;
            }

            return true;
        }

        bool operator!=(const KeyTy& that) const {
            return !this->operator==(that);
        }
    };

    static inline Interface *getEmptyKey() {
        return llvm::DenseMapInfo<Interface *>::getEmptyKey();
    }

    static inline Interface *getTombstoneKey() {
        return llvm::DenseMapInfo<Interface *>::getTombstoneKey();
    }

    static unsigned getHashValue(const KeyTy& key) {
        return llvm::hash_combine(
            key.type,
            llvm::hash_combine_range(key.names.begin(),          key.names.end()),
            llvm::hash_combine_range(key.qualifiedNames.begin(), key.qualifiedNames.end()),
            llvm::hash_combine_range(key.types.begin(),          key.types.end()));
    }

    static unsigned getHashValue(const Interface *interface) {
        return llvm::hash_combine(
            interface->getType(),
            std::accumulate(
                interface->method_begin(), interface->method_end(),
                llvm::hash_code(),
                [](auto current, const auto& method) -> llvm::hash_code {
                    return llvm::hash_combine(current, method.getName());
                }),
            std::accumulate(
                interface->method_begin(), interface->method_end(),
                llvm::hash_code(),
                [](auto current, const auto& method) -> llvm::hash_code {
                    return llvm::hash_combine(current, method.getQualifiedName());
                }),
            std::accumulate(
                interface->method_begin(), interface->method_end(),
                llvm::hash_code(),
                [](auto current, const auto& method) -> llvm::hash_code {
                    return llvm::hash_combine(current, method.getType());
                }));
    }

    static bool isEqual(const KeyTy& lhs, const Interface* rhs) {
        if (rhs == getEmptyKey() || rhs == getTombstoneKey()) {
            return false;
        }

        if (lhs.type != rhs->getType()) {
            return false;
        }

        if (!std::equal(
            lhs.names.begin(), lhs.names.end(),
            rhs->method_begin(), rhs->method_end(),
            [](const auto& lhs, const auto& rhs) -> bool { return lhs == rhs.getName(); })) {
            return false;
        }

        if (!std::equal(
            lhs.qualifiedNames.begin(), lhs.qualifiedNames.end(),
            rhs->method_begin(), rhs->method_end(),
            [](const auto& lhs, const auto& rhs) -> bool { return lhs == rhs.getQualifiedName(); })) {
            return false;
        }

        if (!std::equal(
            lhs.types.begin(), lhs.types.end(),
            rhs->method_begin(), rhs->method_end(),
            [](const auto& lhs, const auto& rhs) -> bool { return lhs == rhs.getType(); })) {
            return false;
        }

        return true;
    }

    static bool isEqual(const Interface* lhs, const Interface *rhs) {
        return lhs == rhs;
    }
};

class LLAIRContextImpl {
public:
    static const LLAIRContextImpl& Get(const LLAIRContext &context) { return *context.d_impl; }
    static LLAIRContextImpl&       Get(LLAIRContext &context) { return *context.d_impl; }

    LLAIRContextImpl(llvm::LLVMContext&);
    ~LLAIRContextImpl();

    const llvm::LLVMContext& getLLContext() const { return d_llcontext; }
    llvm::LLVMContext&       getLLContext()       { return d_llcontext; }

    //
    using ModuleMapType = llvm::DenseMap<llvm::Module *, Module *>;

    ModuleMapType&           modules()       { return d_modules; }
    const ModuleMapType&     modules() const { return d_modules; }

    //
    using EntryPointMapType = llvm::DenseMap<llvm::Function *, EntryPoint *>;

    EntryPointMapType&       entry_points()       { return d_entry_points; }
    const EntryPointMapType& entry_points() const { return d_entry_points; }

    //
    using InterfaceSetType = llvm::DenseSet<Interface *, InterfaceKeyInfo>;

    InterfaceSetType&        interfaces()       { return d_interfaces; }
    const InterfaceSetType&  interfaces() const { return d_interfaces; }

private:
    llvm::LLVMContext& d_llcontext;

    ModuleMapType      d_modules;
    EntryPointMapType  d_entry_points;
    InterfaceSetType   d_interfaces;
};

} // namespace llair

#endif
