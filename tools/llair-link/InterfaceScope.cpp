#include "InterfaceScope.h"

#include <llair/IR/Class.h>
#include <llair/IR/Dispatcher.h>
#include <llair/IR/Module.h>

#include <llvm/IR/DerivedTypes.h>

#include <numeric>

namespace llair {

InterfaceScope::InterfaceScope(llvm::StringRef name, LLAIRContext& context)
: d_module(std::make_unique<llair::Module>(name, context)) {
}

InterfaceScope::~InterfaceScope() {
}

void
InterfaceScope::insertInterface(Interface *interface) {
    auto it_dispatcher = d_dispatchers.insert({ interface, Dispatcher::create(interface, d_module.get()) }).first;

    // Find all classes that match `interface`:
    std::for_each(
        d_klasses.begin(), d_klasses.end(),
        [this, interface, it_dispatcher](auto tmp) {
            auto [ klass, kind ] = tmp;
            auto implemented_method_count = std::count_if(
                klass->method_begin(), klass->method_end(),
                [interface](const auto& method) -> bool {
                    return interface->findMethod(method.getName()) != nullptr;
                });

            if (implemented_method_count != interface->method_size()) {
                return;
            }

            it_dispatcher->second->insertImplementation(kind, klass);
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
    auto& context = d_module->getLLContext();

    auto id = (uint32_t)d_klasses.size();

    auto it_klass = d_klasses.insert({ klass, (uint32_t)d_klasses.size() }).first;

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

            auto it_dispatcher = d_dispatchers.find(interface);
            assert(it_dispatcher != d_dispatchers.end());

            it_dispatcher->second->insertImplementation(it_klass->second, it_klass->first);
        });

    // Add `klass` to the index:
    std::transform(
        klass->method_begin(), klass->method_end(),
        std::inserter(d_klass_index, d_klass_index.end()),
        [klass](const auto& method) -> std::pair<llvm::StringRef, const Class *> {
            return { method.getName(), klass };
        });
}

} // End namespace llair