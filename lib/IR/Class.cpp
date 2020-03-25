#include <llair/IR/Class.h>
#include <llair/IR/Module.h>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>

#include <numeric>

namespace llvm {

template <>
void
SymbolTableListTraits<llair::Class>::addNodeToList(llair::Class *klass) {
    auto owner = getListOwner();
    klass->setModule(owner);
}

template <>
void
SymbolTableListTraits<llair::Class>::removeNodeFromList(llair::Class *klass) {
    klass->setModule(nullptr);
}

} // namespace llvm

namespace llair {

Class::Class(llvm::StructType *type, llvm::ArrayRef<Method> methods, Module *module)
    : d_type(type)
    , d_method_count(methods.size())
    , d_module(module) {
    if (d_module) {
        module->getClassList().push_back(this);
    }

    d_methods = std::allocator<Method>().allocate(d_method_count);

    std::accumulate(methods.begin(), methods.end(),
                    d_methods, [](auto plhs, const auto &rhs) -> auto {
                        new (plhs++) Method(rhs);
                        return plhs;
                    });

    std::sort(d_methods, d_methods + d_method_count, [](const auto &lhs, const auto &rhs) -> auto {
        return lhs.name < rhs.name;
    });
}

Class::~Class() {
    std::for_each(
        d_methods, d_methods + d_method_count,
        [](auto &method) -> void { method.~Method(); });

    std::allocator<Method>().deallocate(d_methods, d_method_count);
}

void
Class::setModule(Module *module) {
    d_module = module;
}

Class *
Class::create(llvm::StructType *type, llvm::ArrayRef<Method> methods, Module *module) {
    auto interface = new Class(type, methods, module);
    return interface;
}

const Class::Method *
Class::findMethod(llvm::StringRef name) const {
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
Class::print(llvm::raw_ostream& os) const {
    os << "class ";
    d_type->print(os, false, true);
    os << " {" << "\n";

    std::for_each(
        d_methods, d_methods + d_method_count,
        [&os](const auto& method) -> void {
            os.indent(4) << method.name << ": ";
            method.function->printAsOperand(os);
            os << "\n";
        });
    os << "}" << "\n";
}

} // End namespace llair