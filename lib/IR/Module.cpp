#include <algorithm>
#include <cstdint>
#include <iostream>
#include <list>
#include <vector>

#include <llair/Demangle/ItaniumDemangle.h>
#include <llair/IR/Class.h>
#include <llair/IR/Dispatcher.h>
#include <llair/IR/EntryPoint.h>
#include <llair/IR/Module.h>

#include <llvm/ADT/Optional.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

#include "LLAIRContextImpl.h"
#include "Metadata.h"

namespace llair {

namespace {

class DefaultAllocator {
    llvm::BumpPtrAllocator Alloc;

public:
    void reset() { Alloc.Reset(); }

    template <typename T, typename... Args> T *makeNode(Args &&... args) {
        return new (Alloc.Allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }

    void *allocateNodeArray(size_t sz) {
        return Alloc.Allocate(sizeof(llair::itanium_demangle::Node *) * sz,
                              alignof(llair::itanium_demangle::Node));
    }
};

using Demangler = llair::itanium_demangle::ManglingParser<DefaultAllocator>;

llvm::Optional<std::tuple<llvm::StringRef, std::vector<llvm::StringRef>, llvm::StringRef>>
parseClassPathAndMethodName(const llvm::Function *function) {
    using namespace llair::itanium_demangle;

    auto name = function->getName();

    Demangler parser(name.begin(), name.end());
    auto      ast = parser.parse();

    if (!ast) {
        return llvm::None;
    }

    if (ast->getKind() != Node::KFunctionEncoding) {
        return llvm::None;
    }

    auto function_encoding = static_cast<FunctionEncoding *>(ast);

    if (function_encoding->getName()->getKind() != Node::KNestedName) {
        return llvm::None;
    }

    auto nested_name = static_cast<const NestedName *>(function_encoding->getName());

    // Class name:
    std::list<llvm::StringRef> path_parts;
    std::size_t path_length = 0;

    for (auto node = nested_name->Qual; node;) {
        StringView identifier;

        switch (node->getKind()) {
        case Node::KNestedName: {
            auto nested_name = static_cast<const NestedName *>(node);

            node       = nested_name->Qual;
            identifier = nested_name->Name->getBaseName();
        } break;
        case Node::KNameType: {
            auto name_type = static_cast<const NameType *>(node);

            node       = nullptr;
            identifier = name_type->getName();
        } break;
        default:
            node = nullptr;
            break;
        }

        path_parts.push_front(llvm::StringRef(identifier.begin(),
                                              std::distance(identifier.begin(), identifier.end())));
        ++path_length;
    }

    llvm::StringRef qualified_name(
        path_parts.front().begin(),
        std::distance(path_parts.front().begin(), path_parts.back().end()));

    std::vector<llvm::StringRef> path;
    path.reserve(path_length);
    std::copy(path_parts.begin(), path_parts.end(),
              std::back_inserter(path));

    // Mangled method name:
    llvm::StringRef method_name(
        nested_name->Name->getBaseName().begin(),
        std::distance(nested_name->Name->getBaseName().begin(), name.end()));

    return std::make_tuple(qualified_name, path, method_name);
}

llvm::Optional<llvm::StructType *>
getSelfType(const llvm::Function *function) {
    auto first_param_type = function->getFunctionType()->getParamType(0);

    auto pointer_type = llvm::dyn_cast<llvm::PointerType>(first_param_type);
    if (!pointer_type) {
        return llvm::None;
    }

    auto self_type = llvm::dyn_cast<llvm::StructType>(pointer_type->getElementType());
    if (!self_type) {
        return llvm::None;
    }

    return self_type;
}

} // namespace

const Module *
Module::Get(const llvm::Module *llmodule) {
    auto &llcontext = llmodule->getContext();
    auto  context   = LLAIRContext::Get(&llcontext);
    if (context) {
        auto it =
            LLAIRContextImpl::Get(*context).modules().find(const_cast<llvm::Module *>(llmodule));
        if (it != LLAIRContextImpl::Get(*context).modules().end()) {
            return it->second;
        }
        else {
            return nullptr;
        }
    }
    else {
        return nullptr;
    }
}

Module *
Module::Get(llvm::Module *llmodule) {
    auto &llcontext = llmodule->getContext();
    auto  context   = LLAIRContext::Get(&llcontext);
    if (context) {
        auto it = LLAIRContextImpl::Get(*context).modules().find(llmodule);
        if (it != LLAIRContextImpl::Get(*context).modules().end()) {
            return it->second;
        }
        else {
            return nullptr;
        }
    }
    else {
        return nullptr;
    }
}

Module::Module(llvm::StringRef id, LLAIRContext &context)
    : d_context(context)
    , d_llmodule(new llvm::Module(id, context.getLLContext())) {
    LLAIRContextImpl::Get(d_context).modules().insert(std::make_pair(d_llmodule.get(), this));

    d_llmodule->setDataLayout(context.getDataLayout());
    d_llmodule->setTargetTriple(context.getTargetTriple());

    setVersion({2, 5, 0});
    setLanguage({"Metal", {3, 0, 0}});
}

Module::Module(std::unique_ptr<llvm::Module> &&module)
    : d_context(*LLAIRContext::Get(&module->getContext()))
    , d_llmodule(std::move(module)) {
    LLAIRContextImpl::Get(d_context).modules().insert(std::make_pair(d_llmodule.get(), this));

    auto version_named_md = d_llmodule->getOrInsertNamedMetadata("air.version");
    if (version_named_md->getNumOperands() > 0) {
        d_version_md.reset(llvm::cast<llvm::MDTuple>(version_named_md->getOperand(0)));
    }

    auto language_named_md = d_llmodule->getOrInsertNamedMetadata("air.language_version");
    if (language_named_md->getNumOperands() > 0) {
        d_language_md.reset(llvm::cast<llvm::MDTuple>(language_named_md->getOperand(0)));
    }

    syncMetadata();
}

Module::~Module() {
    d_dispatchers.clear();
    d_classes.clear();
    d_entry_points.clear();
    LLAIRContextImpl::Get(d_context).modules().erase(d_llmodule.get());
}

std::unique_ptr<llvm::Module>
Module::releaseLLModule() {
    return std::move(d_llmodule);
}

void
Module::setVersion(const Module::Version &version) {
    auto &ll_context = getLLContext();

    d_version_md.reset(llvm::MDTuple::get(
        ll_context, {llvm::ConstantAsMetadata::get(
                         llvm::ConstantInt::get(ll_context, llvm::APInt(32, version.major, true))),
                     llvm::ConstantAsMetadata::get(
                         llvm::ConstantInt::get(ll_context, llvm::APInt(32, version.minor, true))),
                     llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                         ll_context, llvm::APInt(32, version.patch, true)))}));

    auto version_named_md = d_llmodule->getOrInsertNamedMetadata("air.version");
    if (version_named_md->getNumOperands() > 0) {
        version_named_md->setOperand(0, d_version_md.get());
    }
    else {
        version_named_md->addOperand(d_version_md.get());
    }
}

Module::Version
Module::getVersion() const {
    if (d_version_md) {
        auto major = readMDSInt(*d_version_md->getOperand(0)),
             minor = readMDSInt(*d_version_md->getOperand(1)),
             patch = readMDSInt(*d_version_md->getOperand(2));

        if (major && minor && patch) {
            return {*major, *minor, *patch};
        }
    }

    return {0, 0, 0};
}

void
Module::setLanguage(const Module::Language &language) {
    auto &ll_context = getLLContext();

    d_language_md.reset(
        llvm::MDTuple::get(ll_context, {writeMDString(ll_context, language.name),
                                        writeMDInt(ll_context, language.version.major),
                                        writeMDInt(ll_context, language.version.minor),
                                        writeMDInt(ll_context, language.version.patch)}));

    auto language_named_md = d_llmodule->getOrInsertNamedMetadata("air.language_version");
    if (language_named_md->getNumOperands() > 0) {
        language_named_md->setOperand(0, d_language_md.get());
    }
    else {
        language_named_md->addOperand(d_language_md.get());
    }
}

Module::Language
Module::getLanguage() const {
    if (d_language_md) {
        auto name  = readMDString(*d_language_md->getOperand(0));
        auto major = readMDSInt(*d_language_md->getOperand(1)),
             minor = readMDSInt(*d_language_md->getOperand(2)),
             patch = readMDSInt(*d_language_md->getOperand(3));

        if (name && major && minor && patch) {
            return {name->str(), {*major, *minor, *patch}};
        }
    }

    return {"", {0, 0, 0}};
}

EntryPoint *
Module::getEntryPoint(llvm::StringRef name) const {
    auto function = d_llmodule->getFunction(name);
    if (!function) {
        return nullptr;
    }

    return EntryPoint::Get(function);
}

struct InterfaceSpec {
    llvm::StructType *type = nullptr;

    std::vector<llvm::StringRef> method_names;
    std::vector<llvm::StringRef> method_qualified_names;
    std::vector<llvm::FunctionType *> method_types;
};

std::vector<Interface *>
Module::getAllInterfacesFromABI() const {
    llvm::StringMap<InterfaceSpec> interface_specs;

    std::for_each(
        getLLModule()->begin(), getLLModule()->end(),
        [this, &interface_specs](auto &function) -> void {
            if (!function.isDeclarationForLinker()) {
                return;
            }

            auto names = parseClassPathAndMethodName(&function);
            if (!names) {
                return;
            }

            auto interface_name  = std::get<0>(*names);
            auto method_name = std::get<2>(*names);

            auto type = getSelfType(&function);
            if (!type) {
                return;
            }

            auto& interface_spec = interface_specs[interface_name];

            if (!interface_spec.type) {
                interface_spec.type = *type;
            }
            assert(interface_spec.type == *type);

            interface_spec.method_names.push_back(method_name);
            interface_spec.method_qualified_names.push_back(function.getName());
            interface_spec.method_types.push_back(function.getFunctionType());
        });

    std::vector<Interface *> interfaces;
    interfaces.reserve(interface_specs.size());

    std::transform(
        interface_specs.begin(), interface_specs.end(),
        std::back_inserter(interfaces),
        [this](const auto& entry) -> Interface * {
            const auto& interface_spec = entry.getValue();

            return Interface::get(getContext(), interface_spec.type, interface_spec.method_names, interface_spec.method_qualified_names, interface_spec.method_types);
        });

    return interfaces;
}

Class *
Module::getClass(llvm::StringRef name) const {
    auto named = d_class_symbol_table.lookup(name);
    if (!named) {
        return nullptr;
    }

    return static_cast<Class *>(named);
}

struct ClassSpec {
    llvm::StructType *type = nullptr;
    std::vector<llvm::StringRef> method_names;
    std::vector<llvm::Function *> method_functions;

    bool valid() const {
        return type != nullptr && !method_names.empty() && !method_functions.empty() && method_names.size() == method_functions.size();
    }
};

Class *
Module::getOrLoadClassFromABI(llvm::StringRef name) {
    auto klass = getClass(name);
    if (klass) {
        return klass;
    }

    ClassSpec class_spec;

    std::for_each(
        getLLModule()->begin(), getLLModule()->end(),
        [this, name, &class_spec](auto &function) -> void {
            if (!function.isStrongDefinitionForLinker()) {
                return;
            }

            auto names = parseClassPathAndMethodName(&function);
            if (!names) {
                return;
            }

            auto class_name  = std::get<0>(*names);
            auto method_name = std::get<2>(*names);

            if (class_name != name) {
                return;
            }

            auto type = getSelfType(&function);
            if (!type) {
                return;
            }

            if (!class_spec.type) {
                class_spec.type = *type;
            }
            assert(class_spec.type == *type);

            class_spec.method_names.push_back(method_name);
            class_spec.method_functions.push_back(&function);
        });

    if (!class_spec.valid()) {
        return nullptr;
    }

    return Class::Create(class_spec.type, class_spec.method_names, class_spec.method_functions, name, this);
}

std::vector<Class *>
Module::getOrLoadAllClassesFromABI() {
    std::vector<Class *> classes;

    llvm::StringMap<ClassSpec> class_specs;

    std::for_each(
        getLLModule()->begin(), getLLModule()->end(),
        [this, &classes, &class_specs](auto &function) -> void {
            if (!function.isStrongDefinitionForLinker()) {
                return;
            }

            auto names = parseClassPathAndMethodName(&function);
            if (!names) {
                return;
            }

            auto class_name  = std::get<0>(*names);

            auto klass = getClass(class_name);
            if (klass) {
                classes.push_back(klass);
                return;
            }

            auto method_name = std::get<2>(*names);

            auto type = getSelfType(&function);
            if (!type) {
                return;
            }

            auto& class_spec = class_specs[class_name];

            if (!class_spec.type) {
                class_spec.type = *type;
            }
            assert(class_spec.type == *type);

            class_spec.method_names.push_back(method_name);
            class_spec.method_functions.push_back(&function);
        });

    std::for_each(
        class_specs.begin(), class_specs.end(),
        [this, &classes](const auto& entry) -> void {
            auto name = entry.getKey();
            const auto& class_spec = entry.getValue();

            auto named = d_class_symbol_table.lookup(name);
            if (named) {
                return;
            }

            if (!class_spec.valid()) {
                return;
            }

            auto klass = Class::Create(class_spec.type, class_spec.method_names, class_spec.method_functions, name, this);
            classes.push_back(klass);
        });

    return classes;
}

std::pair<Module::DispatcherSetType::const_iterator, Module::DispatcherSetType::const_iterator>
Module::getDispatchers(Interface *interface) const {
    auto it = d_dispatchers_by_interface.find(interface);

    if (it != d_dispatchers_by_interface.end()) {
        return std::make_pair(it->second.begin(), it->second.end());
    }

    return std::make_pair(DispatcherSetType::const_iterator(), DispatcherSetType::const_iterator());
}

std::pair<Module::DispatcherSetType::const_iterator, Module::DispatcherSetType::const_iterator>
Module::getOrInsertDispatchers(Interface *interface) {
    auto range = getDispatchers(interface);
    if (range.first != range.second) {
        return range;
    }

    auto dispatcher = Dispatcher::Create(interface, this);
    auto it = d_dispatchers_by_interface.find(interface);
    assert(it != d_dispatchers_by_interface.end());

    return std::make_pair(it->second.begin(), it->second.end());
}

namespace {

template <typename Input1, typename Input2, typename BinaryFunction1, typename BinaryFunction2,
          typename Compare>
void
for_each_symmetric_difference(Input1 first1, Input1 last1, Input2 first2, Input2 last2,
                              BinaryFunction1 fn1, BinaryFunction2 fn2, Compare comp) {
    while (first1 != last1) {
        if (first2 == last2) {
            std::for_each(first1, last1, fn1);
            return;
        }

        if (comp(*first1, *first2)) {
            fn1(*first1++);
        }
        else {
            if (comp(*first2, *first1)) {
                fn2(*first2);
            }
            else {
                ++first1;
            }
            ++first2;
        }
    }
    std::for_each(first2, last2, fn2);
}

} // namespace

void
Module::syncMetadata() {
    // Entry points:
    {
        auto module = this;

        std::vector<EntryPoint *> entry_points;
        std::transform(d_entry_points.begin(), d_entry_points.end(),
                       std::back_inserter(entry_points),
                       [](auto &entry_point) -> EntryPoint * { return &entry_point; });
        std::sort(entry_points.begin(), entry_points.end(),
                  [](auto lhs, auto rhs) -> bool { return lhs->metadata() < rhs->metadata(); });

        // Vertex:
        {
            auto md = d_llmodule->getNamedMetadata("air.vertex");

            if (md) {
                std::vector<llvm::MDNode *> mds;
                std::copy(md->op_begin(), md->op_end(), std::back_inserter(mds));
                std::sort(mds.begin(), mds.end());

                std::vector<EntryPoint *> vertex_entry_points;
                std::copy_if(entry_points.begin(), entry_points.end(),
                             std::back_inserter(vertex_entry_points), [](auto entry_point) -> bool {
                                 return entry_point->getKind() ==
                                        EntryPoint::EntryPointKind::Vertex;
                             });

                struct Compare {
                    bool operator()(llvm::MDNode *lhs, EntryPoint *rhs) const {
                        return lhs < rhs->metadata();
                    }
                    bool operator()(EntryPoint *lhs, llvm::MDNode *rhs) const {
                        return lhs->metadata() < rhs;
                    }
                };

                for_each_symmetric_difference(
                    mds.begin(), mds.end(), vertex_entry_points.begin(), vertex_entry_points.end(),
                    [&](llvm::MDNode *md) -> void {
                        auto entry_point = new VertexEntryPoint(md, module);
                    },
                    [](EntryPoint *entry_point) -> void {}, Compare());
            }
        }

        // Fragment:
        {
            auto md = d_llmodule->getNamedMetadata("air.fragment");

            if (md) {
                std::vector<llvm::MDNode *> mds;
                std::copy(md->op_begin(), md->op_end(), std::back_inserter(mds));
                std::sort(mds.begin(), mds.end());

                std::vector<EntryPoint *> fragment_entry_points;
                std::copy_if(
                    entry_points.begin(), entry_points.end(),
                    std::back_inserter(fragment_entry_points), [](auto entry_point) -> bool {
                        return entry_point->getKind() == EntryPoint::EntryPointKind::Fragment;
                    });

                struct Compare {
                    bool operator()(llvm::MDNode *lhs, EntryPoint *rhs) const {
                        return lhs < rhs->metadata();
                    }
                    bool operator()(EntryPoint *lhs, llvm::MDNode *rhs) const {
                        return lhs->metadata() < rhs;
                    }
                };

                for_each_symmetric_difference(mds.begin(), mds.end(), fragment_entry_points.begin(),
                                              fragment_entry_points.end(),
                                              [&](llvm::MDNode *md) -> void {
                                                  auto entry_point =
                                                      new FragmentEntryPoint(md, module);
                                              },
                                              [](EntryPoint *entry_point) -> void {}, Compare());
            }
        }

        // Compute:
        {
            auto md = d_llmodule->getNamedMetadata("air.kernel");

            if (md) {
                std::vector<llvm::MDNode *> mds;
                std::copy(md->op_begin(), md->op_end(), std::back_inserter(mds));
                std::sort(mds.begin(), mds.end());

                std::vector<EntryPoint *> compute_entry_points;
                std::copy_if(
                    entry_points.begin(), entry_points.end(),
                    std::back_inserter(compute_entry_points), [](auto entry_point) -> bool {
                        return entry_point->getKind() == EntryPoint::EntryPointKind::Compute;
                    });

                struct Compare {
                    bool operator()(llvm::MDNode *lhs, EntryPoint *rhs) const {
                        return lhs < rhs->metadata();
                    }
                    bool operator()(EntryPoint *lhs, llvm::MDNode *rhs) const {
                        return lhs->metadata() < rhs;
                    }
                };

                for_each_symmetric_difference(mds.begin(), mds.end(), compute_entry_points.begin(),
                                              compute_entry_points.end(),
                                              [&](llvm::MDNode *md) -> void {
                                                  auto entry_point =
                                                      new ComputeEntryPoint(md, module);
                                              },
                                              [](EntryPoint *entry_point) -> void {}, Compare());
            }
        }
    }

    // Classes:
    {
        std::vector<Class *> classes;
        std::transform(
            d_classes.begin(), d_classes.end(),
            std::back_inserter(classes),
            [](auto &klass) -> Class * { return &klass; });
        std::sort(
            classes.begin(), classes.end(),
            [](auto lhs, auto rhs) -> bool { return lhs->metadata() < rhs->metadata(); });

        auto md = d_llmodule->getNamedMetadata("llair.class");

        if (md) {
            struct Compare {
                bool operator()(llvm::Metadata *lhs, Class *rhs) const {
                    return lhs < rhs->metadata();
                }
                bool operator()(Class *lhs, llvm::Metadata *rhs) const {
                    return lhs->metadata() < rhs;
                }
            };

            for_each_symmetric_difference(
                md->op_begin(), md->op_end(),
                classes.begin(), classes.end(),
                [&](llvm::Metadata *md) -> void {
                    Class::Create(md, this);
                },
                [](Class *) -> void {
                },
                Compare());
        }
    }

    // Dispatchers:
    {
        std::vector<Dispatcher *> dispatchers;
        std::transform(
            d_dispatchers.begin(), d_dispatchers.end(),
            std::back_inserter(dispatchers),
            [](auto &dispatcher) -> Dispatcher * { return &dispatcher; });
        std::sort(
            dispatchers.begin(), dispatchers.end(),
            [](auto lhs, auto rhs) -> bool { return lhs->metadata() < rhs->metadata(); });

        auto md = d_llmodule->getNamedMetadata("llair.dispatcher");

        if (md) {
            struct Compare {
                bool operator()(llvm::Metadata *lhs, Dispatcher *rhs) const {
                    return lhs < rhs->metadata();
                }
                bool operator()(Dispatcher *lhs, llvm::Metadata *rhs) const {
                    return lhs->metadata() < rhs;
                }
            };

            for_each_symmetric_difference(
                md->op_begin(), md->op_end(),
                dispatchers.begin(), dispatchers.end(),
                [&](llvm::Metadata *md) -> void {
                    Dispatcher::Create(md, this);
                },
                [](Dispatcher *) -> void {
                },
                Compare());
        }
    }
}

void
Module::print(llvm::raw_ostream& os) const {
    std::for_each(
        entry_point_begin(), entry_point_end(),
        [&os](const auto& entry_point) -> void {
            entry_point.print(os);
        });

    std::for_each(
        class_begin(), class_end(),
        [&os](const auto& klass) -> void {
            klass.print(os);
        });

    std::for_each(
        dispatcher_begin(), dispatcher_end(),
        [&os](const auto& dispatcher) -> void {
            dispatcher.print(os);
        });
}

void
Module::dump() const {
    print(llvm::dbgs());
}

} // End namespace llair
