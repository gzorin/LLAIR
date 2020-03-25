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
class LLVMContext;
class Module;
class SwitchInst;
class Type;
} // End namespace llvm

namespace llair {

class Class;
class Interface;

class InterfaceScope {
public:

    InterfaceScope(llvm::StringRef, llvm::LLVMContext&);
    ~InterfaceScope();

    llvm::Module *module() { return d_module.get(); }

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

    void insertInterface(Interface *);

    std::unique_ptr<llvm::Module> d_module;

    std::unordered_map<const Interface *, std::unique_ptr<Implementations>> d_interfaces;

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

    llvm::DenseSet<Interface *, InterfaceKeyInfo> d_interfaces_;

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

    friend class Interface;
};

} // End namespace llair

#endif