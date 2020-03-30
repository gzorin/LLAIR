#include <llair/IR/Class.h>
#include <llair/IR/Interface.h>
#include <llair/IR/Module.h>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

#include <numeric>

namespace llair {

template<>
void
module_ilist_traits<llair::Class>::addNodeToList(llair::Class *klass) {
    auto module = getModule();
    klass->setModule(module);
}

template<>
void
module_ilist_traits<llair::Class>::removeNodeFromList(llair::Class *klass) {
    klass->setModule(nullptr);
}

Class::Class(llvm::StructType *type, llvm::ArrayRef<llvm::StringRef> names, llvm::ArrayRef<llvm::Function *> functions, llvm::StringRef name, Module *module)
    : d_type(type)
    , d_method_count(std::min(names.size(), functions.size())) {
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

    setName(name);

    if (module) {
        module->getClassList().push_back(this);
    }
    assert(d_module == module);
}

Class::~Class() {
    std::for_each(
        d_methods, d_methods + d_method_count,
        [](auto &method) -> void { method.~Method(); });

    std::allocator<Method>().deallocate(d_methods, d_method_count);
}

void
Class::setModule(Module *module) {
    if (d_module == module) {
        return;
    }

    if (d_module) {
        setSymbolTable(nullptr);
    }

    d_module = module;

    if (d_module) {
        setSymbolTable(&d_module->getClassSymbolTable());
    }
}

Class *
Class::create(llvm::StructType *type, llvm::ArrayRef<llvm::StringRef> names, llvm::ArrayRef<llvm::Function *> functions, llvm::StringRef name, Module *module) {
    auto interface = new Class(type, names, functions, name, module);
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

bool
Class::doesImplement(const Interface *interface) const {
    auto it_interface_method = interface->method_begin();
    auto it_klass_method = method_begin();
    std::size_t count = 0;

    while (it_interface_method != interface->method_end() && it_klass_method != method_end()) {
        if (it_interface_method->getName() < it_klass_method->getName()) {
            ++it_interface_method;
        } else  {
            if (!(it_klass_method->getName() < it_interface_method->getName())) {
                assert(it_klass_method->getName() == it_interface_method->getName());

                ++it_interface_method;
                ++count;
            }
            ++it_klass_method;
        }
    }

    return count == interface->method_size();
}

void
Class::print(llvm::raw_ostream& os) const {
    os << "class ";
    if (hasName()) {
        os << getName() << " ";
    }
    os << "(";
    d_type->print(os, false, true);
    os << ") {" << "\n";

    std::for_each(
        d_methods, d_methods + d_method_count,
        [&os](const auto& method) -> void {
            os.indent(4) << method.getName() << ": ";
            method.getFunction()->printAsOperand(os);
            os << "\n";
        });
    os << "}" << "\n";
}

void
Class::dump() const {
    print(llvm::dbgs());
}

LLAIRContext&
Class::getContext() const {
    return *LLAIRContext::Get(&d_type->getContext());
}

Class::Method::Method(llvm::StringRef name, llvm::Function *function)
: d_name(name)
, d_function(function) {
}

} // End namespace llair