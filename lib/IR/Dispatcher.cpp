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

namespace llair {

template<>
void
module_ilist_traits<llair::Dispatcher>::addNodeToList(llair::Dispatcher *dispatcher) {
    auto module = getModule();
    dispatcher->setModule(module);
}

template<>
void
module_ilist_traits<llair::Dispatcher>::removeNodeFromList(llair::Dispatcher *dispatcher) {
    dispatcher->setModule(nullptr);
}

Dispatcher::Dispatcher(Interface *interface, Module *module)
: d_interface(interface) {
    auto method_count = method_size();

    d_methods = std::allocator<Method>().allocate(method_count);

    auto p_method = d_methods;
    auto it_interface_method = d_interface->method_begin();

    std::vector<llvm::Metadata *> method_mds;
    method_mds.reserve(method_count);

    for (auto n = method_count; n > 0; --n, ++p_method, ++it_interface_method) {
        auto method = new (p_method) Method(d_interface, it_interface_method);
        method_mds.push_back(method->d_md.get());
    }

    auto& ll_context = d_interface->getContext().getLLContext();

    d_implementations_md.reset(llvm::MDTuple::get(ll_context, {}));

    d_md.reset(llvm::MDTuple::get(
        ll_context,
        { d_interface->metadata(),
          llvm::MDTuple::get(ll_context, method_mds),
          d_implementations_md.get() } ));

    if (module) {
        module->getDispatcherList().push_back(this);
    }
    assert(d_module == module);
}

Dispatcher::Dispatcher(llvm::Metadata *md, Module *module) {
    if (module) {
        module->getDispatcherList().push_back(this);
    }
    assert(d_module == module);

    d_md.reset(llvm::cast<llvm::MDTuple>(md));

    auto interface_md = d_md->getOperand(0).get();

    d_interface = Interface::get(interface_md);

    auto method_count = method_size();

    auto methods_md = llvm::cast<llvm::MDTuple>(d_md->getOperand(1).get());

    d_methods = std::allocator<Method>().allocate(method_count);

    auto p_method = d_methods;
    auto it_interface_method = d_interface->method_begin();
    auto it_methods_md = methods_md->op_begin();

    for (auto n = method_count; n > 0; --n, ++p_method, ++it_interface_method, ++it_methods_md) {
        auto method = new (p_method) Method(it_methods_md->get(), it_interface_method);
    }

    d_implementations_md.reset(llvm::cast<llvm::MDTuple>(d_md->getOperand(2).get()));

    std::for_each(
        d_implementations_md->op_begin(), d_implementations_md->op_end(),
        [this](auto &operand) -> void {
            auto implementation_md = llvm::cast<llvm::MDTuple>(operand.get());

            auto kind = llvm::mdconst::extract<llvm::ConstantInt>(implementation_md->getOperand(0).get())->getZExtValue();
            auto name = llvm::cast<llvm::MDString>(implementation_md->getOperand(1).get())->getString();

            d_implementations.insert({ kind, { name } });
        });
}

Dispatcher::~Dispatcher() {
    auto method_count = method_size();

    std::for_each(
        d_methods, d_methods + method_count,
        [](auto &method) -> void { method.~Method(); });

    std::allocator<Method>().deallocate(d_methods, method_count);
}

void
Dispatcher::setModule(Module *module) {
    if (d_module == module) {
        return;
    }

    if (d_module) {
        auto method_count = method_size();

        std::for_each(
            d_methods, d_methods + method_count,
            [this](auto &method) -> void {
                d_module->getLLModule()->getFunctionList().remove(method.getFunction());
            });

        d_module->d_dispatchers_by_interface[d_interface].erase(this);

        if (d_module->getLLModule() && d_md) {
            auto dispatchers_md = d_module->getLLModule()->getNamedMetadata("llair.dispatcher");

            if (dispatchers_md) {
                auto it =
                    std::find(dispatchers_md->op_begin(), dispatchers_md->op_end(), d_md.get());
                if (it != dispatchers_md->op_end()) {
                    dispatchers_md->setOperand(std::distance(dispatchers_md->op_begin(), it),
                                               nullptr);
                }
            }
        }
    }

    d_module = module;

    if (d_module) {
        auto method_count = method_size();

        std::for_each(
            d_methods, d_methods + method_count,
            [this](auto &method) -> void { d_module->getLLModule()->getFunctionList().push_back(method.getFunction()); });

        d_module->d_dispatchers_by_interface[d_interface].insert(this);

        if (d_module->getLLModule() && d_md) {
            auto dispatchers_md = d_module->getLLModule()->getOrInsertNamedMetadata("llair.dispatcher");
            dispatchers_md->addOperand(d_md.get());
        }
    }
}

void
Dispatcher::updateImplementationsMetadata() {
    auto& ll_context = d_interface->getContext().getLLContext();

    std::vector<llvm::Metadata *> mds;
    mds.reserve(d_implementations.size());

    std::transform(
        d_implementations.begin(), d_implementations.end(),
        std::back_inserter(mds),
        [&ll_context](const auto& tmp) -> llvm::Metadata * {
            auto [ kind, implementation ] = tmp;

            return llvm::MDTuple::get(ll_context,
                { llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                        ll_context, llvm::APInt(32, kind, true))),
                  llvm::MDString::get(ll_context, implementation.name) });
        });

    d_implementations_md.reset(llvm::MDTuple::get(ll_context, mds));

    d_md->replaceOperandWith(2, d_implementations_md.get());
}

Dispatcher *
Dispatcher::Create(Interface *interface, Module *module) {
    auto dispatcher = new Dispatcher(interface, module);
    return dispatcher;
}

Dispatcher *
Dispatcher::Create(llvm::Metadata *md, Module *module) {
    auto dispatcher = new Dispatcher(md, module);
    return dispatcher;
}

std::size_t
Dispatcher::method_size() const {
    return d_interface ? d_interface->method_size() : 0;
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

    auto tmp = std::equal_range(d_methods, d_methods + method_size(), name, Compare());

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
    updateImplementationsMetadata();

    auto& ll_context = d_interface->getContext().getLLContext();

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

                if (it_method->d_switcher->getDefaultDest()->empty()) {
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

                //
                std::vector<llvm::Type *> params;
                auto it_params = std::back_inserter(params);

                // `that` type:
                *it_params++ = *it_klass_method->getFunction()->getFunctionType()->param_begin();

                // Other params:
                std::copy(
                    it_method->d_function->getFunctionType()->param_begin() + 1, it_method->d_function->getFunctionType()->param_end(),
                    it_params);

                auto klass_function_type = llvm::FunctionType::get(
                    it_method->d_function->getReturnType(), params, false);

                auto klass_function = d_module->getLLModule()->getOrInsertFunction(
                    it_klass_method->getFunction()->getName(), klass_function_type);

                //
                std::vector<llvm::Value *> args;
                args.reserve(klass_function_type->getNumParams());
                auto it_args = std::back_inserter(args);

                // `that`:
                auto that = builder->CreateStructGEP(
                    nullptr,
                    builder->CreatePointerCast(
                        it_method->d_function->arg_begin(),
                        llvm::PointerType::get(type, 1)), 1);

                *it_args++ = that;

                // Other arguments:
                std::transform(
                    it_method->d_function->arg_begin() + 1, it_method->d_function->arg_end(),
                    it_args,
                    [](auto& arg) -> llvm::Value * {
                        return &arg;
                    });

                auto call = builder->CreateCall(klass_function, args);

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

Dispatcher::Method::Method(Interface *interface, const Interface::Method *interface_method)
: d_interface_method(interface_method) {
    d_function = llvm::Function::Create(
        d_interface_method->getType(), llvm::GlobalValue::ExternalLinkage, d_interface_method->getQualifiedName(),
        nullptr);

    auto& ll_context = d_function->getContext();

    auto builder = std::make_unique<llvm::IRBuilder<>>(ll_context);

    builder->SetInsertPoint(
        llvm::BasicBlock::Create(ll_context, "entry", d_function));

    auto abstract_that = d_function->arg_begin();

    auto kind = builder->CreateLoad(
        builder->CreateStructGEP(nullptr, abstract_that, 0));

    d_switcher = builder->CreateSwitch(kind,
        llvm::BasicBlock::Create(ll_context, "", d_function));

    d_md.reset(llvm::ConstantAsMetadata::get(d_function));
}

Dispatcher::Method::Method(llvm::Metadata *md, const Interface::Method *interface_method)
: d_interface_method(interface_method) {
    d_md.reset(llvm::cast<llvm::ConstantAsMetadata>(md));

    d_function = llvm::mdconst::extract<llvm::Function>(d_md.get());

    d_switcher = llvm::cast<llvm::SwitchInst>(d_function->getEntryBlock().getTerminator());
}

Dispatcher::Method::~Method() {
    assert(d_function->getParent() == nullptr);

    std::for_each(
        d_function->use_begin(), d_function->use_end(),
        [](auto& use) -> void {
            use.set(nullptr);
        });

    delete d_function;
}

llvm::Function *
Dispatcher::Method::getFunction() const {
    return llvm::mdconst::extract<llvm::Function>(d_md.get());
}

} // End namespace llair