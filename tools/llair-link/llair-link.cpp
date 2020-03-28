#include <llair/Bitcode/Bitcode.h>
#include <llair/IR/Class.h>
#include <llair/IR/Dispatcher.h>
#include <llair/IR/Interface.h>
#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Linker/Linker.h>

#include "InterfaceScope.h"

#include <llvm/Demangle/ItaniumDemangle.h>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/ToolOutputFile.h>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

#include <algorithm>
#include <iostream>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace {

llvm::cl::list<std::string> input_filenames(llvm::cl::Positional, llvm::cl::ZeroOrMore,
                                            llvm::cl::desc("<input .bc files>"));

llvm::cl::opt<std::string> output_filename("o", llvm::cl::Required,
                                           llvm::cl::desc("Override output filename"),
                                           llvm::cl::value_desc("filename"));

} // namespace

class DefaultAllocator {
    llvm::BumpPtrAllocator Alloc;

public:
    void reset() { Alloc.Reset(); }

    template <typename T, typename... Args> T *makeNode(Args &&... args) {
        return new (Alloc.Allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }

    void *allocateNodeArray(size_t sz) {
        return Alloc.Allocate(sizeof(llvm::itanium_demangle::Node *) * sz,
                              alignof(llvm::itanium_demangle::Node));
    }
};

using Demangler = llvm::itanium_demangle::ManglingParser<DefaultAllocator>;

std::string
make_string(llvm::itanium_demangle::StringView string_view) {
    return std::string(string_view.begin(), string_view.end());
}

std::string
getBaseName(const llvm::itanium_demangle::Node *node) {
    auto base_name = node->getBaseName();
    return std::string(base_name.begin(), base_name.end());
}

std::optional<std::tuple<llvm::StringRef, std::vector<llvm::StringRef>, llvm::StringRef>>
parseClassPathAndMethodName(const llvm::Function *function) {
    using namespace llvm::itanium_demangle;

    auto name = function->getName();

    Demangler parser(name.begin(), name.end());
    auto      ast = parser.parse();

    if (!ast) {
        return {};
    }

    if (ast->getKind() != Node::KFunctionEncoding) {
        return {};
    }

    auto function_encoding = static_cast<FunctionEncoding *>(ast);

    if (function_encoding->getName()->getKind() != Node::KNestedName) {
        return {};
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

std::optional<llvm::StructType *>
getSelfType(llvm::Function *function) {
    auto first_param_type = function->getFunctionType()->getParamType(0);

    auto pointer_type = llvm::dyn_cast<llvm::PointerType>(first_param_type);
    if (!pointer_type) {
        return {};
    }

    auto self_type = llvm::dyn_cast<llvm::StructType>(pointer_type->getElementType());
    if (!self_type) {
        return {};
    }

    return self_type;
}

std::string
canonicalizeClassPath(llvm::ArrayRef<llvm::StringRef> class_path) {
    auto length =
        std::accumulate(class_path.begin(), class_path.end(), (class_path.size() - 1) * 2,
                        [](std::size_t length, llvm::StringRef identifier) -> std::size_t {
                            return length + identifier.size();
                        });

    std::string name(length, 0);

    auto class_path_it = class_path.begin();
    auto name_it       = name.begin();

    name_it = std::copy(class_path_it->begin(), class_path_it->end(), name_it);

    std::accumulate(
        ++class_path_it, class_path.end(), name_it,
        [](std::string::iterator name_it, llvm::StringRef identifier) -> std::string::iterator {
            static const std::string k_delimiter("::");

            name_it = std::copy(k_delimiter.begin(), k_delimiter.end(), name_it);

            return std::copy(identifier.begin(), identifier.end(), name_it);
        });

    return name;
}

class Implementation;

using namespace llair;

//
class Namespace {
public:

    using Path = std::vector<llvm::StringRef>;

    using Member = std::variant<Namespace *, Interface *, Class *>;

    std::optional<Interface *> insert(Path::const_iterator, Path::const_iterator, std::unique_ptr<Interface>);
    std::optional<Class *> insert(Path::const_iterator, Path::const_iterator, std::unique_ptr<Class>);

    std::size_t erase(Path::const_iterator, Path::const_iterator);

    std::optional<Member> find(Path::const_iterator, Path::const_iterator) const;

private:

    using Node = std::variant<std::monostate,
                              std::unique_ptr<Namespace>,
                              std::unique_ptr<Interface>,
                              std::unique_ptr<Class>>;

    static std::optional<Member> assign(const Node&);

    using Nodes = std::unordered_map<std::string, Node>;

    std::optional<Node *> insert(Path::const_iterator, Path::const_iterator);

    Nodes d_nodes;
};

std::optional<Namespace::Member>
Namespace::assign(const Node& node) {
    struct Extract {
        std::optional<Member> operator()(std::monostate) const {
            return {};
        }

        std::optional<Member> operator()(const std::unique_ptr<Namespace>& value) const {
            return value.get();
        }

        std::optional<Member> operator()(const std::unique_ptr<Interface>& value) const {
            return value.get();
        }

        std::optional<Member> operator()(const std::unique_ptr<Class>& value) const {
            return value.get();
        }
    };

    return std::visit(Extract(), node);
}

std::optional<Interface *>
Namespace::insert(Path::const_iterator it, Path::const_iterator it_end, std::unique_ptr<Interface> interface) {
    auto node = insert(it, it_end);
    if (!node) {
        return {};
    }

    **node = std::move(interface);

    return std::get<std::unique_ptr<Interface>>(**node).get();
}

std::optional<Class *>
Namespace::insert(Path::const_iterator it, Path::const_iterator it_end, std::unique_ptr<Class> klass) {
    auto node = insert(it, it_end);
    if (!node) {
        return {};
    }

    **node = std::move(klass);

    return std::get<std::unique_ptr<Class>>(**node).get();
}

std::optional<Namespace::Node *>
Namespace::insert(Path::const_iterator it, Path::const_iterator it_end) {
    Namespace *that = this;
    Nodes::iterator it_node;

    while (it != it_end) {
        assert(that);

        auto& nodes = that->d_nodes;
        auto identifier = *it++;

        // Find a node for `identifier`:
        it_node = nodes.find(identifier);

        // If there is no node, insert a new uninitialized one:
        if (it_node == nodes.end()) {
            it_node = nodes.insert({ identifier.str(), Node() }).first;
        }

        // In the middle of the path, so it_node should hold a `Namespace`:
        if (it != it_end) {
            // Uninitialized non-terminal node should be initialized with a new `Namespace`:
            if (std::holds_alternative<std::monostate>(it_node->second)) {
                it_node->second = std::make_unique<Namespace>();
            }
            // Initialized non-terminal node does not hold a `Namespace`, so give up:
            else if (!std::holds_alternative<std::unique_ptr<Namespace>>(it_node->second)) {
                return nullptr;
            }

            that = std::get<std::unique_ptr<Namespace>>(it_node->second).get();
        }
    }

    return &it_node->second;
}

std::optional<Namespace::Member>
Namespace::find(Path::const_iterator it, Path::const_iterator it_end) const {
    const Namespace *that = this;
    Nodes::const_iterator it_node;

    while (it != it_end) {
        assert(that);

        auto& nodes = that->d_nodes;
        auto identifier = *it++;

        // Find a node for `identifier`:
        it_node = nodes.find(identifier);

        // If there is no node, then there is nothing to find:
        if (it_node == nodes.end()) {
            return {};
        }

        // In the middle of the path, so it_node should hold a `Namespace`:
        if (it != it_end) {
            // Non-terminal node does not hold a `Namespace`, so give up:
            if (!std::holds_alternative<std::unique_ptr<Namespace>>(it_node->second)) {
                return {};
            }

            that = std::get<std::unique_ptr<Namespace>>(it_node->second).get();
        }
    }

    return assign(it_node->second);
}

std::size_t
Namespace::erase(Path::const_iterator it, Path::const_iterator it_end) {
    Namespace *that = this;
    Nodes::const_iterator it_node;

    while (it != it_end) {
        assert(that);

        auto& nodes = that->d_nodes;
        auto identifier = *it++;

        // Find a node for `identifier`:
        it_node = nodes.find(identifier);

        // If there is no node, then there is nothing to erase:
        if (it_node == nodes.end()) {
            return 0;
        }

        // In the middle of the path, so it_node should hold a `Namespace`:
        if (it != it_end) {
            // Non-terminal node does not hold a `Namespace`, so give up:
            if (!std::holds_alternative<std::unique_ptr<Namespace>>(it_node->second)) {
                return 0;
            }

            that = std::get<std::unique_ptr<Namespace>>(it_node->second).get();
        }
    }

    that->d_nodes.erase(it_node);

    return 1;
}

//
int
main(int argc, char **argv) {
    llvm::cl::ParseCommandLineOptions(argc, argv, "llair-link\n");

    llvm::ExitOnError exit_on_err("llosl-link: ");

    auto llvm_context  = std::make_unique<llvm::LLVMContext>();
    auto llair_context = std::make_unique<llair::LLAIRContext>(*llvm_context);

    std::vector<std::unique_ptr<llair::Module>> input_modules;

    std::transform(
        input_filenames.begin(), input_filenames.end(), std::back_inserter(input_modules),
        [&exit_on_err, &llair_context](auto input_filename) -> std::unique_ptr<llair::Module> {
            auto buffer =
                exit_on_err(errorOrToExpected(llvm::MemoryBuffer::getFileOrSTDIN(input_filename)));
            auto module = exit_on_err(
                llair::getBitcodeModule(llvm::MemoryBufferRef(*buffer), *llair_context));
            return module;
        });

    auto interface_scope = std::make_unique<InterfaceScope>(output_filename, *llair_context);

    // Discover interfaces:
    // - with list of modules, and optional list of interface names
    // - iterate over all modules, collect declared and defined functions into two different sets
    // - functions that are 'declared' and not 'defined' potentially specify interfaces
    // - for each function that is 'declared' and not 'defined':
    //    - if it is not part of a class, ignore it
    //    - if interface names is not null & class name is not in it, ignore it
    //
    //    - get or create an interface with the class name
    //    - add the function type, indexed by its mangled leaf name
    //    - get the struct type of the first parameter, add it to the set of types that
    //      represent this interface (since named types are not uniqued)
    //    - associate the name of the function with the interface
    std::vector<llvm::Function *> declared_functions, defined_functions;

    std::for_each(
        input_modules.begin(), input_modules.end(),
        [&declared_functions, &defined_functions](auto &input_module) -> void {
            std::for_each(
                input_module->getLLModule()->begin(), input_module->getLLModule()->end(),
                [&declared_functions, &defined_functions](auto &function) -> void {
                    if (function.isDeclarationForLinker()) {
                        declared_functions.push_back(&function);
                    }
                    else if (function.isStrongDefinitionForLinker()) {
                        defined_functions.push_back(&function);
                    }
                });
        });

    std::sort(declared_functions.begin(), declared_functions.end(), [](auto lhs, auto rhs) -> bool {
        return lhs->getName().compare(rhs->getName()) < 0;
    });

    std::sort(defined_functions.begin(), defined_functions.end(), [](auto lhs, auto rhs) -> bool {
        return lhs->getName().compare(rhs->getName()) < 0;
    });

    std::vector<llvm::Function *> undefined_functions;

    std::set_difference(
        declared_functions.begin(), declared_functions.end(),
        defined_functions.begin(), defined_functions.end(),
        std::back_inserter(undefined_functions),
        [](auto lhs, auto rhs) -> bool { return lhs->getName().compare(rhs->getName()) < 0; });

    struct InterfaceSpec {
        std::vector<llvm::StringRef> path;
        llvm::StructType *type = nullptr;

        struct Method {
            std::string name, qualifiedName;
            llvm::FunctionType *type = nullptr;
        };

        struct Compare {
            bool operator()(const Method& lhs, const Method& rhs) const {
                return lhs.name < rhs.name;
            }
        };

        std::set<Method, Compare> methods;
    };

    std::unordered_map<std::string, InterfaceSpec> interface_specs;

    std::for_each(
        undefined_functions.begin(), undefined_functions.end(),
        [&interface_specs](auto function) -> void {
            auto names = parseClassPathAndMethodName(function);
            if (!names) {
                return;
            }

            auto type = getSelfType(function);
            if (!type) {
                return;
            }

            auto class_name  = canonicalizeClassPath(std::get<1>(*names));
            auto method_name = std::get<2>(*names);

            auto it = interface_specs.find(class_name);
            if (it == interface_specs.end()) {
                it = interface_specs.insert({ class_name, {} }).first;

                it->second.path = std::get<1>(*names);
                it->second.type = *type;
            }

            it->second.methods.insert({ method_name.str(), function->getName().str(), function->getFunctionType() });
        });

    std::for_each(
        interface_specs.begin(), interface_specs.end(),
        [&llair_context, &interface_scope](const auto& tmp) -> void {
            auto [ key, interface_spec ] = tmp;

            std::vector<llvm::StringRef> names, qualifiedNames;
            std::vector<llvm::FunctionType *> types;
            names.reserve(interface_spec.methods.size());
            qualifiedNames.reserve(interface_spec.methods.size());
            types.reserve(interface_spec.methods.size());

            std::accumulate(
                interface_spec.methods.begin(), interface_spec.methods.end(),
                std::make_tuple(std::back_inserter(names), std::back_inserter(qualifiedNames), std::back_inserter(types)),
                [](auto it, const auto& method) -> auto {
                    auto [ it_name, it_qualifiedName, it_type ] = it;

                    *it_name++ = method.name;
                    *it_qualifiedName++ = method.qualifiedName;
                    *it_type++ = method.type;

                    return make_tuple(it_name, it_qualifiedName, it_type);
                });

            auto interface = Interface::get(*llair_context, interface_spec.type, names, qualifiedNames, types);
            interface->print(llvm::errs());

            interface_scope->insertInterface(interface);
        });

    //
    // Discover implementations:
    // - for each module:
    //   - for each function that is 'defined':
    //     - if it is not part of a class, ignore it
    //     - if the function name is not associated with any interface, ignore it
    //
    //
    std::vector<const Class *> klasses;

    struct ClassSpec {
        llvm::StringRef name;
        std::vector<llvm::StringRef> path;
        llvm::StructType *type = nullptr;

        struct Method {
            std::string name;
            llvm::Function *function = nullptr;
        };

        std::vector<Method> methods;

        Module *module = nullptr;
    };

    std::for_each(
        input_modules.begin(), input_modules.end(),
        [&interface_scope, &klasses](auto &input_module) -> void {
            std::unordered_map<std::string, ClassSpec> class_specs;

            std::for_each(
                input_module->getLLModule()->begin(), input_module->getLLModule()->end(),
                [&input_module, &class_specs](auto& function) -> void {
                    if (!function.isStrongDefinitionForLinker()) {
                        return;
                    }

                    auto names = parseClassPathAndMethodName(&function);
                    if (!names) {
                        return;
                    }

                    auto type = getSelfType(&function);
                    if (!type) {
                        return;
                    }

                    auto class_name  = canonicalizeClassPath(std::get<1>(*names));
                    auto method_name = std::get<2>(*names);

                    auto it = class_specs.find(class_name);
                    if (it == class_specs.end()) {
                        it = class_specs.insert({ class_name, {} }).first;

                        it->second.name = std::get<0>(*names);
                        it->second.path = std::get<1>(*names);
                        it->second.type = *type;
                        it->second.module = input_module.get();
                    }

                    it->second.methods.push_back({ method_name.str(), &function });
                });

            std::for_each(
                class_specs.begin(), class_specs.end(),
                [&interface_scope, &klasses](const auto& tmp) -> void {
                    auto [ key, class_spec ] = tmp;

                    std::vector<llvm::StringRef> names;
                    std::vector<llvm::Function *> functions;
                    names.reserve(class_spec.methods.size());
                    functions.reserve(class_spec.methods.size());

                    std::accumulate(
                        class_spec.methods.begin(), class_spec.methods.end(),
                        std::make_tuple(std::back_inserter(names), std::back_inserter(functions)),
                        [](auto it, const auto& method) -> auto {
                            auto [ it_name, it_type ] = it;

                            *it_name++ = method.name;
                            *it_type++ = method.function;

                            return make_tuple(it_name, it_type);
                        });

                    auto klass = Class::create(class_spec.type, names, functions, class_spec.name, class_spec.module);
                    klass->print(llvm::errs());

                    interface_scope->insertClass(klass);
                });
        });

    std::for_each(
        interface_scope->module()->getDispatcherList().begin(), interface_scope->module()->getDispatcherList().end(),
        [](const auto& dispatcher) -> void {
            dispatcher.dump();
        });

    // Write it out:
    std::error_code                       error_code;
    std::unique_ptr<llvm::ToolOutputFile> output_file(
        new llvm::ToolOutputFile(output_filename, error_code, llvm::sys::fs::F_None));

    if (error_code) {
        llvm::errs() << error_code.message();
        return 1;
    }

    llvm::WriteBitcodeToFile(interface_scope->module()->getLLModule(), output_file->os());
    output_file->keep();

    return 0;
}
