#include <llair/IR/Interface.h>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

#include <numeric>

#include "LLAIRContextImpl.h"

namespace llair {

Interface::Interface(LLAIRContext& context, llvm::StructType *type, llvm::ArrayRef<llvm::StringRef> names, llvm::ArrayRef<llvm::StringRef> qualifiedNames, llvm::ArrayRef<llvm::FunctionType *> types)
    : d_context(context)
    , d_type(type)
    , d_method_count(std::min(std::min(names.size(), qualifiedNames.size()), types.size())) {
    d_methods = std::allocator<Method>().allocate(d_method_count);

    auto p_method = d_methods;
    auto it_name = names.begin();
    auto it_qualifiedName = qualifiedNames.begin();
    auto it_type = types.begin();

    for (auto n = d_method_count; n > 0; --n, ++p_method, ++it_name, ++it_qualifiedName, ++it_type) {
        new (p_method) Method(*it_name, *it_qualifiedName, *it_type);
    }

    std::sort(
        d_methods, d_methods + d_method_count,
        [](const auto &lhs, const auto &rhs) -> auto {
            return lhs.getName() < rhs.getName();
        });
}

Interface::~Interface() {
    std::for_each(
        d_methods, d_methods + d_method_count,
        [](auto &method) -> void { method.~Method(); });

    std::allocator<Method>().deallocate(d_methods, d_method_count);
}

Interface *
Interface::get(LLAIRContext& context, llvm::StructType *type, llvm::ArrayRef<llvm::StringRef> names, llvm::ArrayRef<llvm::StringRef> qualifiedNames, llvm::ArrayRef<llvm::FunctionType *> types) {
    auto& context_impl = LLAIRContextImpl::Get(context);

    auto it = context_impl.interfaces().find_as(
        InterfaceKeyInfo::KeyTy(type, names, qualifiedNames, types));

    if (it != context_impl.interfaces().end()) {
        return *it;
    }

    auto interface = new Interface(context, type, names, qualifiedNames, types);
    context_impl.interfaces().insert(interface);
    return interface;
}

const Interface::Method *
Interface::findMethod(llvm::StringRef name) const {
    struct Compare {
        bool operator()(llvm::StringRef lhs, const Method &rhs) const {
            return lhs.compare(rhs.getName()) < 0;
        }

        bool operator()(const Method &lhs, llvm::StringRef rhs) const {
            return rhs.compare(lhs.getName()) >= 0;
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
            os.indent(4) << method.getName() << ": ";
            method.getType()->print(os);
            os << "\n";
        });
    os << "}" << "\n";
}

void
Interface::dump() const {
    print(llvm::dbgs());
}

Interface::Method::Method(llvm::StringRef name, llvm::StringRef qualifiedName, llvm::FunctionType *type)
: d_name(name)
, d_qualifiedName(qualifiedName)
, d_type(type) {
}

} // End namespace llair