#include <llair/Bitcode/Bitcode.h>
#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Linker/Linker.h>

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

std::optional<std::tuple<std::vector<llvm::StringRef>, llvm::StringRef>>
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

    std::vector<llvm::StringRef> path;
    path.reserve(path_length);
    std::copy(path_parts.begin(), path_parts.end(),
              std::back_inserter(path));

    // Mangled method name:
    llvm::StringRef method_name(
        nested_name->Name->getBaseName().begin(),
        std::distance(nested_name->Name->getBaseName().begin(), name.end()));

    return std::make_tuple(path, method_name);
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

class Class;
class Namespace;
class Implementation;
class Interface;

//
class Interface {
public:
    struct Method {
        std::string         name;
        std::string         qualifiedName;
        llvm::FunctionType *type = nullptr;
    };

    static Interface *get(llvm::StructType *, llvm::ArrayRef<Method>);

    ~Interface();

    llvm::StructType *getType() const { return d_type; }

    using method_iterator       = Method *;
    using const_method_iterator = const Method *;

    method_iterator       method_begin() { return d_methods; }
    const method_iterator method_begin() const { return d_methods; }

    method_iterator       method_end() { return d_methods + method_size(); }
    const method_iterator method_end() const { return d_methods + method_size(); }

    std::size_t method_size() const { return d_method_count; }

    const Method *findMethod(llvm::StringRef) const;

    void print(llvm::raw_ostream&) const;

private:
    Interface(llvm::StructType *, llvm::ArrayRef<Method>);

    llvm::StructType *d_type = nullptr;

    std::size_t d_method_count = 0;
    Method *    d_methods      = nullptr;
};

Interface::Interface(llvm::StructType *type, llvm::ArrayRef<Method> methods)
    : d_type(type)
    , d_method_count(methods.size()) {
    d_methods = std::allocator<Method>().allocate(d_method_count);

    std::accumulate(
        methods.begin(), methods.end(),
        d_methods,
        [](auto plhs, const auto &rhs) -> auto {
            new (plhs++) Method(rhs);
            return plhs;
        });

    std::sort(d_methods, d_methods + d_method_count, [](const auto &lhs, const auto &rhs) -> auto {
        return lhs.name < rhs.name;
    });
}

Interface::~Interface() {
    std::for_each(
        d_methods, d_methods + d_method_count,
        [](auto &method) -> void { method.~Method(); });

    std::allocator<Method>().deallocate(d_methods, d_method_count);
}

Interface *
Interface::get(llvm::StructType *type, llvm::ArrayRef<Method> methods) {
    auto interface = new Interface(type, methods);
    return interface;
}

const Interface::Method *
Interface::findMethod(llvm::StringRef name) const {
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
Interface::print(llvm::raw_ostream& os) const {
    os << "interface ";
    d_type->print(os, false, true);
    os << " {" << "\n";

    std::for_each(
        d_methods, d_methods + d_method_count,
        [&os](const auto& method) -> void {
            os.indent(4) << method.name << ": ";
            method.type->print(os);
            os << "\n";
        });
    os << "}" << "\n";
}

//
class Class {
public:
    struct Method {
        std::string      name;
        llvm::Function  *function = nullptr;
    };

    static Class *create(llvm::StructType *, llvm::ArrayRef<Method>);

    ~Class();

    llvm::StructType *getType() const { return d_type; }

    using method_iterator       = Method *;
    using const_method_iterator = const Method *;

    method_iterator       method_begin() { return d_methods; }
    const method_iterator method_begin() const { return d_methods; }

    method_iterator       method_end() { return d_methods + method_size(); }
    const method_iterator method_end() const { return d_methods + method_size(); }

    std::size_t method_size() const { return d_method_count; }

    const Method *findMethod(llvm::StringRef) const;

    void print(llvm::raw_ostream&) const;

private:

    Class(llvm::StructType *, llvm::ArrayRef<Method>);

    llvm::StructType *d_type = nullptr;

    std::size_t d_method_count = 0;
    Method *    d_methods      = nullptr;
};

Class::Class(llvm::StructType *type, llvm::ArrayRef<Method> methods)
    : d_type(type)
    , d_method_count(methods.size()) {
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

Class *
Class::create(llvm::StructType *type, llvm::ArrayRef<Method> methods) {
    auto interface = new Class(type, methods);
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
class InterfaceScope {
public:

    InterfaceScope(llvm::StringRef, llvm::LLVMContext&);

    llvm::Module *module() { return d_module.get(); }

    void insertInterface(const Interface *);
    void insertClass(const Class *);

    class Implementations {
    public:

        struct Method {
            llvm::Function  *function = nullptr;

        private:

            Method(llvm::Function *function, llvm::SwitchInst *switcher)
            : function(function)
            , switcher(switcher) {
            }

            llvm::SwitchInst *switcher = nullptr;

            friend class Implementations;
        };

        ~Implementations();

        const Interface *interface() const { return d_interface; }

        using method_iterator       = Method *;
        using const_method_iterator = const Method *;

        method_iterator       method_begin() { return d_methods; }
        const method_iterator method_begin() const { return d_methods; }

        method_iterator       method_end() { return d_methods + method_size(); }
        const method_iterator method_end() const { return d_methods + method_size(); }

        std::size_t method_size() const { return d_interface->method_size(); }

    private:

        static std::unique_ptr<Implementations> create(const Interface *, llvm::Module *);

        Implementations(const Interface *, llvm::Module *);

        void addImplementation(const Class *, uint32_t, llvm::StructType *, llvm::ArrayRef<llvm::Function *>, llvm::Module *);

        const Interface *d_interface = nullptr;

        Method *d_methods = nullptr;

        friend class InterfaceScope;
    };

private:

    std::unique_ptr<llvm::Module> d_module;

    std::unordered_map<const Interface *, std::unique_ptr<Implementations>> d_interfaces;

    struct ClassData {
        uint32_t id = 0;
        llvm::StructType *type = nullptr;
        std::vector<llvm::Function *> method_functions;
    };

    std::unordered_map<const Class *, ClassData> d_klasses;

    struct Hash {
        std::size_t operator()(llvm::StringRef name) const {
            return llvm::hash_value(name);
        }
    };

    std::unordered_multimap<llvm::StringRef, const Interface *, Hash> d_interface_index;
    std::unordered_multimap<llvm::StringRef, const Class *, Hash > d_klass_index;
};

InterfaceScope::InterfaceScope(llvm::StringRef name, llvm::LLVMContext& ll_context)
: d_module(std::make_unique<llvm::Module>(name, ll_context)) {
}

void
InterfaceScope::insertInterface(const Interface *interface) {
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
                    return interface->findMethod(method.name) != nullptr;
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
            return { method.name, interface };
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
                method.function->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
                method.function->getName(), d_module.get());
        });

    auto it_klass = d_klasses.insert({ klass, { (uint32_t)d_klasses.size(), type, method_functions } }).first;

    // Find all interfaces that match `klass`:
    std::unordered_map<const Interface *, std::size_t> implemented_method_count;

    std::for_each(
        it_klass->first->method_begin(), it_klass->first->method_end(),
        [this, it_klass, &implemented_method_count](const auto& method) {
            auto it = d_interface_index.equal_range(method.name);

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
            return { method.name, klass };
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
                abstract_method.type, llvm::GlobalValue::ExternalLinkage, abstract_method.qualifiedName,
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
        if (it_interface_method->name < it_klass_method->name) {
            ++it_interface_method;
            ++it_method;
        } else  {
            if (!(it_klass_method->name < it_interface_method->name)) {
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
                args.reserve(it_klass_method->function->arg_size());

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

    Namespace global_namespace;

    auto interface_scope = std::make_unique<InterfaceScope>(output_filename, *llvm_context);

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
    std::vector<const Interface *> interfaces;

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

        struct Compare {
            bool operator()(const Interface::Method& lhs, const Interface::Method& rhs) const {
                return lhs.name < rhs.name;
            }
        };

        std::set<Interface::Method, Compare> methods;
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

            auto class_name  = canonicalizeClassPath(std::get<0>(*names));
            auto method_name = std::get<1>(*names);

            auto it = interface_specs.find(class_name);
            if (it == interface_specs.end()) {
                it = interface_specs.insert({ class_name, {} }).first;

                it->second.path = std::get<0>(*names);
                it->second.type = *type;
            }

            it->second.methods.insert({ method_name.str(), function->getName().str(), function->getFunctionType() });
        });

    std::for_each(
        interface_specs.begin(), interface_specs.end(),
        [&global_namespace, &interface_scope, &interfaces](const auto& tmp) -> void {
            auto [ key, interface_spec ] = tmp;

            std::vector<Interface::Method> methods;
            methods.reserve(interface_spec.methods.size());

            std::copy(
                interface_spec.methods.begin(), interface_spec.methods.end(),
                std::back_inserter(methods));

            std::unique_ptr<Interface> interface(Interface::get(interface_spec.type, methods));
            interface->print(llvm::errs());

            auto result = global_namespace.insert(interface_spec.path.begin(), interface_spec.path.end(), std::move(interface));

            if (!result) {
                return;
            }

            interface_scope->insertInterface(*result);

            interfaces.push_back(*result);
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
        std::vector<llvm::StringRef> path;
        llvm::StructType *type = nullptr;

        std::vector<Class::Method> methods;
    };

    std::for_each(
        input_modules.begin(), input_modules.end(),
        [&global_namespace, &interface_scope, &klasses](auto &input_module) -> void {
            std::unordered_map<std::string, ClassSpec> class_specs;

            std::for_each(
                input_module->getLLModule()->begin(), input_module->getLLModule()->end(),
                [&class_specs](auto& function) -> void {
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

                    auto class_name  = canonicalizeClassPath(std::get<0>(*names));
                    auto method_name = std::get<1>(*names);

                    auto it = class_specs.find(class_name);
                    if (it == class_specs.end()) {
                        it = class_specs.insert({ class_name, {} }).first;

                        it->second.path = std::get<0>(*names);
                        it->second.type = *type;
                    }

                    it->second.methods.push_back({ method_name.str(), &function });
                });

            std::for_each(
                class_specs.begin(), class_specs.end(),
                [&global_namespace, &interface_scope, &klasses](const auto& tmp) -> void {
                    auto [ key, class_spec ] = tmp;

                    std::unique_ptr<Class> klass(Class::create(class_spec.type, class_spec.methods));
                    klass->print(llvm::errs());

                    auto result = global_namespace.insert(class_spec.path.begin(), class_spec.path.end(), std::move(klass));
                    if (!result) {
                        return;
                    }

                    interface_scope->insertClass(*result);

                    klasses.push_back(*result);
                });
        });

    // Write it out:
    std::error_code                       error_code;
    std::unique_ptr<llvm::ToolOutputFile> output_file(
        new llvm::ToolOutputFile(output_filename, error_code, llvm::sys::fs::F_None));

    if (error_code) {
        llvm::errs() << error_code.message();
        return 1;
    }

    llvm::WriteBitcodeToFile(interface_scope->module(), output_file->os());
    output_file->keep();

    return 0;
}
