#include "InterfaceScope.h"

#include <llair/IR/Class.h>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include <numeric>

namespace llair {

InterfaceScope::InterfaceScope(llvm::StringRef name, llvm::LLVMContext& ll_context)
: d_module(std::make_unique<llvm::Module>(name, ll_context)) {
}

InterfaceScope::~InterfaceScope() {
    std::for_each(
        d_interfaces_.begin(), d_interfaces_.end(),
        [](auto interface) {
            delete interface;
        });
}

void
InterfaceScope::insertInterface(Interface *interface) {
    d_interfaces_.insert(interface);

    auto implementations = Implementations::create(interface, d_module.get());

    auto it_interface = d_interfaces.insert({ interface, std::move(implementations) }).first;

    // Find all classes that match `interface`:
    std::for_each(
        d_klasses.begin(), d_klasses.end(),
        [this, interface, it_interface](auto tmp) {
            auto [ klass, data ] = tmp;
            auto implemented_method_count = std::count_if(
                klass->method_begin(), klass->method_end(),
                [interface](const auto& method) -> bool {
                    return interface->findMethod(method.getName()) != nullptr;
                });

            if (implemented_method_count != interface->method_size()) {
                return;
            }

            it_interface->second->addImplementation(klass, data.id, data.type, data.method_functions, d_module.get());
        });

    // Add `interface` to the index:
    std::transform(
        interface->method_begin(), interface->method_end(),
        std::inserter(d_interface_index, d_interface_index.end()),
        [interface](const auto& method) -> std::pair<llvm::StringRef, const Interface *>{
            return { method.getName(), interface };
        });
}

void
InterfaceScope::insertClass(const Class *klass) {
    auto& context = d_module->getContext();

    auto id = (uint32_t)d_klasses.size();
    auto type = llvm::StructType::get(
        context, std::vector<llvm::Type *>{
            llvm::Type::getInt32Ty(context),
            klass->getType()
        });

    std::vector<llvm::Function *> method_functions;
    method_functions.reserve(klass->method_size());

    std::transform(
        klass->method_begin(), klass->method_end(),
        std::back_inserter(method_functions),
        [this](const auto& method) -> llvm::Function * {
            return llvm::Function::Create(
                method.getFunction()->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
                method.getFunction()->getName(), d_module.get());
        });

    auto it_klass = d_klasses.insert({ klass, { (uint32_t)d_klasses.size(), type, method_functions } }).first;

    // Find all interfaces that match `klass`:
    std::unordered_map<const Interface *, std::size_t> implemented_method_count;

    std::for_each(
        it_klass->first->method_begin(), it_klass->first->method_end(),
        [this, it_klass, &implemented_method_count](const auto& method) {
            auto it = d_interface_index.equal_range(method.getName());

            std::for_each(
                it.first, it.second,
                [&implemented_method_count](auto tmp) {
                    auto interface = tmp.second;
                    implemented_method_count[interface]++;
                });
        });

    std::for_each(
        implemented_method_count.begin(), implemented_method_count.end(),
        [this, it_klass](auto tmp) {
            auto [ interface, implemented_method_count ] = tmp;
            if (implemented_method_count != interface->method_size()) {
                return;
            }

            auto it_interface = d_interfaces.find(interface);
            assert(it_interface != d_interfaces.end());

            it_interface->second->addImplementation(it_klass->first, it_klass->second.id, it_klass->second.type, it_klass->second.method_functions, d_module.get());
        });

    // Add `klass` to the index:
    std::transform(
        klass->method_begin(), klass->method_end(),
        std::inserter(d_klass_index, d_klass_index.end()),
        [klass](const auto& method) -> std::pair<llvm::StringRef, const Class *> {
            return { method.getName(), klass };
        });
}

InterfaceScope::Implementations::Implementations(const Interface *interface, llvm::Module *module)
: d_interface(interface) {
    auto& context = module->getContext();

    auto method_count = d_interface->method_size();

    d_methods = std::allocator<Method>().allocate(method_count);

    auto abstract_that_type = llvm::StructType::get(context, std::vector<llvm::Type *>{
        llvm::Type::getInt32Ty(context)
    });

    std::accumulate(
        interface->method_begin(), interface->method_end(),
        d_methods,
        [module, &context, abstract_that_type](auto pconcrete_method, const auto &abstract_method) -> auto {
            auto builder = std::make_unique<llvm::IRBuilder<>>(context);

            auto function = llvm::Function::Create(
                abstract_method.getType(), llvm::GlobalValue::ExternalLinkage, abstract_method.getQualifiedName(),
                module);

            auto block = llvm::BasicBlock::Create(context, "entry", function);
            builder->SetInsertPoint(block);

            auto abstract_that = builder->CreatePointerCast(
                function->arg_begin(), llvm::PointerType::get(abstract_that_type, 1));

            auto kind = builder->CreateLoad(
                builder->CreateStructGEP(nullptr, abstract_that, 0));

            auto exit = llvm::BasicBlock::Create(context, "", function);

            auto switcher = builder->CreateSwitch(kind, exit);

            new (pconcrete_method++) Method(function, switcher);
            return pconcrete_method;
        });
}

InterfaceScope::Implementations::~Implementations() {
    auto method_count = d_interface->method_size();

    std::for_each(
        d_methods, d_methods + method_count,
        [](auto &method) -> void { method.~Method(); });

    std::allocator<Method>().deallocate(d_methods, method_count);
}

std::unique_ptr<InterfaceScope::Implementations>
InterfaceScope::Implementations::create(const Interface *interface, llvm::Module *module) {
    std::unique_ptr<Implementations> result(new Implementations(interface, module));
    return result;
}

void
InterfaceScope::Implementations::addImplementation(const Class *klass, uint32_t id, llvm::StructType *type, llvm::ArrayRef<llvm::Function *> method_functions, llvm::Module *module) {
    auto& context = module->getContext();

    auto it_interface_method = d_interface->method_begin();
    auto it_method = d_methods;
    auto it_klass_method = klass->method_begin();
    auto it_method_function = method_functions.begin();

    while (it_interface_method != d_interface->method_end() && it_klass_method != klass->method_end()) {
        if (it_interface_method->getName() < it_klass_method->getName()) {
            ++it_interface_method;
            ++it_method;
        } else  {
            if (!(it_klass_method->getName() < it_interface_method->getName())) {
                auto builder = std::make_unique<llvm::IRBuilder<>>(context);

                if (it_method->switcher->getNumCases() == 0) {
                    builder->SetInsertPoint(it_method->switcher->getDefaultDest());
                }
                else {
                    auto block = llvm::BasicBlock::Create(context, "", it_method->function);

                    it_method->switcher->addCase(
                        llvm::ConstantInt::get(
                            llvm::Type::getInt32Ty(context), id, false), block);

                    builder->SetInsertPoint(block);
                }

                auto that = builder->CreateStructGEP(
                    nullptr,
                    builder->CreatePointerCast(
                        it_method->function->arg_begin(),
                        llvm::PointerType::get(type, 1)), 1);

                std::vector<llvm::Value *> args;
                args.reserve(it_klass_method->getFunction()->arg_size());

                auto it_args = std::back_inserter(args);

                *it_args++ = that;

                std::transform(
                    it_method->function->arg_begin() + 1, it_method->function->arg_end(),
                    it_args,
                    [](auto& arg) -> auto {
                        return &arg;
                    });

                auto call = builder->CreateCall(
                    *it_method_function, args);

                builder->CreateRet(call);

                ++it_interface_method;
                ++it_method;
            }
            ++it_klass_method;
            ++it_method_function;
        }
    }
}

} // End namespace llair