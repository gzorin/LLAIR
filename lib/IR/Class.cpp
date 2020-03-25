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

Class::Class(llvm::StructType *type, llvm::ArrayRef<llvm::StringRef> names, llvm::ArrayRef<llvm::Function *> functions, Module *module)
    : d_type(type)
    , d_method_count(std::min(names.size(), functions.size()))
    , d_module(module) {
    if (d_module) {
        module->getClassList().push_back(this);
    }

    d_methods = std::allocator<Method>().allocate(d_method_count);

    auto p_method = d_methods;
    auto it_name = names.begin();
    auto it_function = functions.begin();

    for (auto n = d_method_count; n > 0; --n, ++p_method, ++it_name, ++it_function) {
        new (p_method) Method(*it_name, *it_function);
    }

    std::sort(d_methods, d_methods + d_method_count, [](const auto &lhs, const auto &rhs) -> auto {
        return lhs.getName() < rhs.getName();
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
Class::create(llvm::StructType *type, llvm::ArrayRef<llvm::StringRef> names, llvm::ArrayRef<llvm::Function *> functions, Module *module) {
    auto interface = new Class(type, names, functions, module);
    return interface;
}

const Class::Method *
Class::findMethod(llvm::StringRef name) const {
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
Class::print(llvm::raw_ostream& os) const {
    os << "class ";
    d_type->print(os, false, true);
    os << " {" << "\n";

    std::for_each(
        d_methods, d_methods + d_method_count,
        [&os](const auto& method) -> void {
            os.indent(4) << method.getName() << ": ";
            method.getFunction()->printAsOperand(os);
            os << "\n";
        });
    os << "}" << "\n";
}

Class::Method::Method(llvm::StringRef name, llvm::Function *function)
: d_name(name)
, d_function(function) {
}

} // End namespace llair