#include <llair/IR/Class.h>
#include <llair/IR/Dispatcher.h>
#include <llair/IR/Interface.h>
#include <llair/IR/Module.h>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

#include <numeric>

namespace llvm {

template <>
void
SymbolTableListTraits<llair::Dispatcher>::addNodeToList(llair::Dispatcher *dispatcher) {
    auto owner = getListOwner();
    dispatcher->setModule(owner);
}

template <>
void
SymbolTableListTraits<llair::Dispatcher>::removeNodeFromList(llair::Dispatcher *dispatcher) {
    dispatcher->setModule(nullptr);
}

} // namespace llvm

namespace llair {

Dispatcher::Dispatcher(const Interface *interface, Module *module)
    : d_interface(interface)
    , d_method_count(d_interface->method_size()) {
    if (module) {
        module->getDispatcherList().push_back(this);
    }
    assert(d_module == module);

    d_methods = std::allocator<Method>().allocate(d_method_count);

    auto p_method = d_methods;
    auto it_interface_method = d_interface->method_begin();

    for (auto n = d_method_count; n > 0; --n, ++p_method, ++it_interface_method) {
        new (p_method) Method(d_interface, it_interface_method, d_module);
    }
}

Dispatcher::~Dispatcher() {
    std::for_each(
        d_methods, d_methods + d_method_count,
        [](auto &method) -> void { method.~Method(); });

    std::allocator<Method>().deallocate(d_methods, d_method_count);
}

void
Dispatcher::setModule(Module *module) {
    if (d_module == module) {
        return;
    }

#if 0
    if (d_module) {
        std::for_each(
            d_methods, d_methods + d_method_count,
            [](auto &method) -> void { method.getFunction()->removeFromParent(); });
    }
#endif

    d_module = module;

#if 0
    if (d_module) {
        std::for_each(
            d_methods, d_methods + d_method_count,
            [this](auto &method) -> void { d_module->getLLModule()->getFunctionList().push_back(method.getFunction()); });
    }
#endif
}

Dispatcher *
Dispatcher::create(const Interface *interface, Module *module) {
    auto dispatcher = new Dispatcher(interface, module);
    return dispatcher;
}

const Dispatcher::Method *
Dispatcher::findMethod(llvm::StringRef name) const {
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
Dispatcher::insertImplementation(uint32_t kind, const Class *klass) {
    assert(klass->doesImplement(d_interface));

    auto it_implementation = d_implementations.find(kind);
    assert(it_implementation == d_implementations.end());
    it_implementation = d_implementations.insert({ kind, { klass->getName().str() } }).first;

    auto& ll_context = d_module->getLLContext();

    auto type = llvm::StructType::get(
        ll_context, std::vector<llvm::Type *>{
            llvm::Type::getInt32Ty(ll_context),
            klass->getType()
        });

    auto it_interface_method = d_interface->method_begin();
    auto it_method = d_methods;
    auto it_klass_method = klass->method_begin();

    while (it_interface_method != d_interface->method_end() && it_klass_method != klass->method_end()) {
        if (it_interface_method->getName() < it_klass_method->getName()) {
            ++it_interface_method;
            ++it_method;
        } else  {
            if (!(it_klass_method->getName() < it_interface_method->getName())) {
                assert(it_klass_method->getName() == it_interface_method->getName());

                auto builder = std::make_unique<llvm::IRBuilder<>>(ll_context);

                llvm::BasicBlock *block = nullptr;

                if (it_method->d_switcher->getNumCases() == 0) {
                    block = it_method->d_switcher->getDefaultDest();
                }
                else {
                    block = llvm::BasicBlock::Create(ll_context, "", it_method->d_function);

                    it_method->d_switcher->addCase(
                        llvm::ConstantInt::get(
                            llvm::Type::getInt32Ty(ll_context), kind, false), block);
                }

                block->setName(klass->getName());
                builder->SetInsertPoint(block);

                auto that = builder->CreateStructGEP(
                    nullptr,
                    builder->CreatePointerCast(
                        it_method->d_function->arg_begin(),
                        llvm::PointerType::get(type, 1)), 1);

                std::vector<llvm::Value *> args;
                args.reserve(it_klass_method->getFunction()->arg_size());

                auto it_args = std::back_inserter(args);

                *it_args++ = that;

                std::transform(
                    it_method->d_function->arg_begin() + 1, it_method->d_function->arg_end(),
                    it_args,
                    [](auto& arg) -> auto {
                        return &arg;
                    });

                auto method_function = d_module->getLLModule()->getOrInsertFunction(
                    it_klass_method->getFunction()->getName(), it_klass_method->getFunction()->getFunctionType());

                auto call = builder->CreateCall(method_function, args);

                builder->CreateRet(call);

                ++it_interface_method;
                ++it_method;
            }
            ++it_klass_method;
        }
    }
}

void
Dispatcher::removeImplementation(uint32_t kind) {
}

void
Dispatcher::print(llvm::raw_ostream& os) const {
    os << "dispatcher ";
    os << "(";
    d_interface->getType()->print(os, false, true);
    os << ") {" << "\n";

    std::for_each(
        d_implementations.begin(), d_implementations.end(),
        [&os](const auto& tmp) -> void {
            auto [ kind, implementation ] = tmp;
            os.indent(4) << kind << ": " << implementation.name << "\n";
        });

    os << "}" << "\n";
}

void
Dispatcher::dump() const {
    print(llvm::dbgs());
}

Dispatcher::Method::Method(const Interface *interface, const Interface::Method *interface_method, Module *module)
: d_interface_method(interface_method) {
    auto& ll_context = interface->getContext().getLLContext();

    auto abstract_that_type = llvm::StructType::get(ll_context, std::vector<llvm::Type *>{
        llvm::Type::getInt32Ty(ll_context)
    });

    d_function = llvm::Function::Create(
        d_interface_method->getType(), llvm::GlobalValue::ExternalLinkage, d_interface_method->getQualifiedName(),
        module ? module->getLLModule() : nullptr);

    auto builder = std::make_unique<llvm::IRBuilder<>>(ll_context);

    builder->SetInsertPoint(
        llvm::BasicBlock::Create(ll_context, "entry", d_function));

    auto abstract_that = builder->CreatePointerCast(
        d_function->arg_begin(), llvm::PointerType::get(abstract_that_type, 1));

    auto kind = builder->CreateLoad(
        builder->CreateStructGEP(nullptr, abstract_that, 0));

    d_switcher = builder->CreateSwitch(kind,
        llvm::BasicBlock::Create(ll_context, "", d_function));
}

} // End namespace llair