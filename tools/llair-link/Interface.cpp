#include "Interface.h"
#include "InterfaceScope.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/Support/raw_ostream.h>

#include <numeric>

namespace llair {

Interface::Interface(InterfaceScope& scope, llvm::StructType *type, llvm::ArrayRef<Method> methods)
    : d_scope(scope)
    , d_type(type)
    , d_method_count(methods.size()) {
    d_methods = std::allocator<Method>().allocate(d_method_count);

    std::accumulate(
        methods.begin(), methods.end(),
        d_methods,
        [](auto plhs, const auto &rhs) -> auto {
            new (plhs++) Method(rhs);
            return plhs;
        });

    std::sort(d_methods, d_methods + d_method_count, [](const auto &lhs, const auto &rhs) -> auto {
        return lhs.name < rhs.name;
    });
}

Interface::~Interface() {
    std::for_each(
        d_methods, d_methods + d_method_count,
        [](auto &method) -> void { method.~Method(); });

    std::allocator<Method>().deallocate(d_methods, d_method_count);
}

Interface *
Interface::get(InterfaceScope& scope, llvm::StructType *type, llvm::ArrayRef<Method> methods) {
    auto it = scope.d_interfaces_.find_as(
        InterfaceScope::InterfaceKeyInfo::KeyTy(type, methods));

    if (it != scope.d_interfaces_.end()) {
        return *it;
    }

    auto interface = new Interface(scope, type, methods);
    scope.insertInterface(interface);
    return interface;
}

const Interface::Method *
Interface::findMethod(llvm::StringRef name) const {
    struct Compare {
        bool operator()(llvm::StringRef lhs, const Method &rhs) const {
            return lhs.compare(rhs.name) < 0;
        }

        bool operator()(const Method &lhs, llvm::StringRef rhs) const {
            return rhs.compare(lhs.name) >= 0;
        }
    };

    auto tmp = std::equal_range(d_methods, d_methods + d_method_count, name, Compare());

    if (tmp.first == tmp.second) {
        return nullptr;
    }

    return tmp.first;
}

void
Interface::print(llvm::raw_ostream& os) const {
    os << "interface ";
    d_type->print(os, false, true);
    os << " {" << "\n";

    std::for_each(
        d_methods, d_methods + d_method_count,
        [&os](const auto& method) -> void {
            os.indent(4) << method.name << ": ";
            method.type->print(os);
            os << "\n";
        });
    os << "}" << "\n";
}

} // End namespace llair