#include <llair/IR/Class.h>
#include <llair/IR/Interface.h>
#include <llair/IR/Module.h>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
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
    auto& ll_context = d_type->getContext();

    d_type_with_kind = llvm::StructType::get(
        ll_context, std::vector<llvm::Type *>{
            llvm::Type::getInt32Ty(ll_context),
            d_type
        });

    d_methods = std::allocator<Method>().allocate(d_method_count);

    auto p_method = d_methods;
    auto it_name = names.begin();
    auto it_function = functions.begin();

    for (auto n = d_method_count; n > 0; --n, ++p_method, ++it_name, ++it_function) {
        new (p_method) Method(*it_name, *it_function);
    }

    std::sort(
        d_methods, d_methods + d_method_count,
        [](const auto &lhs, const auto &rhs) -> auto {
            return lhs.getName() < rhs.getName();
        });

    std::vector<llvm::Metadata *> method_mds;
    method_mds.reserve(d_method_count);

    std::transform(
        d_methods, d_methods + d_method_count,
        std::back_inserter(method_mds),
        [](const auto& method) -> auto {
            return method.d_md.get();
        });

    setName(name);

    d_md.reset(llvm::MDTuple::get(
        ll_context,
        { llvm::MDString::get(ll_context, getName()),
          llvm::ConstantAsMetadata::get(
                llvm::ConstantPointerNull::get(llvm::PointerType::get(d_type, 0))),
          llvm::MDTuple::get(ll_context, method_mds) } ));

    if (module) {
        module->getClassList().push_back(this);
    }
    assert(d_module == module);
}

Class::Class(llvm::Metadata *md, Module *module) {
    if (module) {
        module->getClassList().push_back(this);
    }
    assert(d_module == module);

    d_md.reset(llvm::cast<llvm::MDTuple>(md));

    d_type = llvm::cast<llvm::StructType>(
        llvm::mdconst::extract<llvm::ConstantPointerNull>(d_md->getOperand(1).get())->
            getType()->
                getElementType());

    auto& ll_context = d_type->getContext();

    d_type_with_kind = llvm::StructType::get(
        ll_context, std::vector<llvm::Type *>{
            llvm::Type::getInt32Ty(ll_context),
            d_type
        });

    updateLayout();

    auto methods_md = llvm::cast<llvm::MDTuple>(d_md->getOperand(2).get());

    d_method_count = methods_md->getNumOperands();
    d_methods = std::allocator<Method>().allocate(d_method_count);

    auto p_method = d_methods;
    auto it_methods_md = methods_md->op_begin();

    for (auto n = d_method_count; n > 0; --n, ++p_method, ++it_methods_md) {
        auto method = new (p_method) Method(it_methods_md->get());
    }

    auto name_md = llvm::cast<llvm::MDString>(d_md->getOperand(0).get());
    setName(name_md->getString());
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

        d_size.reset();
        d_size_with_kind.reset();
        d_offset_past_kind.reset();

        if (d_module->getLLModule() && d_md) {
            auto class_md = d_module->getLLModule()->getNamedMetadata("llair.class");

            if (class_md) {
                auto it =
                    std::find(class_md->op_begin(), class_md->op_end(), d_md.get());
                if (it != class_md->op_end()) {
                    class_md->setOperand(std::distance(class_md->op_begin(), it),
                                         nullptr);
                }
            }
        }
    }

    d_module = module;

    if (d_module) {
        setSymbolTable(&d_module->getClassSymbolTable());

        updateLayout();

        if (d_module->getLLModule() && d_md) {
            auto dispatchers_md = d_module->getLLModule()->getOrInsertNamedMetadata("llair.class");
            dispatchers_md->addOperand(d_md.get());
        }
    }
}

void
Class::updateLayout() {
    if (d_module->getLLModule() && d_type) {
        auto layout = d_module->getLLModule()->getDataLayout().getStructLayout(d_type);
        assert(layout);
        d_size = layout->getSizeInBytes();
    }

    if (d_module->getLLModule() && d_type_with_kind) {
        auto layout = d_module->getLLModule()->getDataLayout().getStructLayout(d_type_with_kind);
        assert(layout);
        d_size_with_kind = layout->getSizeInBytes();
        d_offset_past_kind = layout->getElementOffset(1);
    }
}

Class *
Class::Create(llvm::StructType *type, llvm::ArrayRef<llvm::StringRef> names, llvm::ArrayRef<llvm::Function *> functions, llvm::StringRef name, Module *module) {
    auto klass = new Class(type, names, functions, name, module);
    return klass;
}

Class *
Class::Create(llvm::Metadata *md, Module *module) {
    auto klass = new Class(md, module);
    return klass;
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

Class::Method::Method(llvm::StringRef name, llvm::Function *function) {
    auto& ll_context = function->getFunctionType()->getContext();

    d_md.reset(llvm::MDTuple::get(ll_context,
        { llvm::MDString::get(ll_context, name),
          llvm::ConstantAsMetadata::get(function) } ));
}

Class::Method::Method(llvm::Metadata *md) {
    d_md.reset(llvm::cast<llvm::MDTuple>(md));
}

llvm::StringRef
Class::Method::getName() const {
    return llvm::cast<llvm::MDString>(d_md->getOperand(0).get())->getString();
}

llvm::Function *
Class::Method::getFunction() const {
    return llvm::mdconst::extract<llvm::Function>(d_md->getOperand(1).get());
}

} // End namespace llair