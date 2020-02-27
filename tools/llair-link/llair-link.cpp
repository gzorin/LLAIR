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
#include <llvm/Support/ToolOutputFile.h>

#include <algorithm>
#include <iostream>
#include <list>
#include <variant>
#include <vector>
#include <string>

namespace {

llvm::cl::list<std::string> input_filenames(llvm::cl::Positional, llvm::cl::ZeroOrMore,
					  llvm::cl::desc("<input .bc files>"));

llvm::cl::opt<std::string> output_filename("o", llvm::cl::Required,
					   llvm::cl::desc("Override output filename"),
					   llvm::cl::value_desc("filename"));

}

class DefaultAllocator {
  llvm::BumpPtrAllocator Alloc;

public:
  void reset() { Alloc.Reset(); }

  template<typename T, typename ...Args> T *makeNode(Args &&...args) {
    return new (Alloc.Allocate(sizeof(T), alignof(T)))
        T(std::forward<Args>(args)...);
  }

  void *allocateNodeArray(size_t sz) {
    return Alloc.Allocate(sizeof(llvm::itanium_demangle::Node *) * sz, alignof(llvm::itanium_demangle::Node));
  }
};

using Demangler = llvm::itanium_demangle::ManglingParser<DefaultAllocator>;

std::string
make_string(llvm::itanium_demangle::StringView string_view) {
    return std::string(string_view.begin(), string_view.end());
}

std::string
getBaseName(const llvm::itanium_demangle::Node* node) {
    auto base_name = node->getBaseName();
    return std::string(base_name.begin(), base_name.end());
}

std::optional<std::tuple<std::list<llvm::StringRef>, llvm::StringRef> >
parseClassPathAndMethodName(const llvm::Function *function) {
    using namespace llvm::itanium_demangle;

    auto name = function->getName();

    Demangler parser(name.begin(), name.end());
    auto ast = parser.parse();

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
    std::list<llvm::StringRef> class_name;

    for (auto node = nested_name->Qual; node; ) {
        StringView identifier;

        switch (node->getKind()) {
            case Node::KNestedName: {
                auto nested_name = static_cast<const NestedName *>(node);

                node = nested_name->Qual;
                identifier = nested_name->Name->getBaseName();
            } break;
            case Node::KNameType: {
                auto name_type = static_cast<const NameType *>(node);

                node = nullptr;
                identifier = name_type->getName();
            } break;
            default:
                node = nullptr;
                break;
        }

        class_name.push_front(llvm::StringRef(identifier.begin(),
                                             std::distance(identifier.begin(), identifier.end())));
    }

    // Mangled method name:
    llvm::StringRef method_name(nested_name->Name->getBaseName().begin(),
                                std::distance(nested_name->Name->getBaseName().begin(), name.end()));

    return std::make_tuple(class_name, method_name);
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
canonicalizeClassPath(const std::list<llvm::StringRef>& class_path) {
    auto length =
        std::accumulate(
            class_path.begin(), class_path.end(),
            (class_path.size() - 1) * 2,
            [](std::size_t length, llvm::StringRef identifier) -> std::size_t {
                return length + identifier.size();
            });

    std::string name(length, 0);

    auto class_path_it = class_path.begin();
    auto name_it = name.begin();

    name_it = std::copy(
        class_path_it->begin(), class_path_it->end(),
        name_it);

    std::accumulate(
        class_path_it++, class_path.end(),
        name_it,
        [](std::string::iterator name_it, llvm::StringRef identifier) -> std::string::iterator {
            static const std::string k_delimiter("::");

            name_it = std::copy(
                k_delimiter.begin(), k_delimiter.end(),
                name_it);

            return std::copy(
                identifier.begin(), identifier.end(),
                name_it);
        });

    return name;
}

struct Class;
struct Namespace;
struct Implementation;
struct Interface;

class Interface {
public:

    struct Method {
        std::string name;
        std::string qualifiedName;
        llvm::FunctionType *type = nullptr;
    };

    static Interface *get(llvm::StructType *, llvm::ArrayRef<Method>);

    ~Interface();

    llvm::StructType *getType() const { return d_type; }

    using method_iterator = Method *;
    using const_method_iterator = const Method *;

    method_iterator method_begin() { return d_methods; }
    const method_iterator method_begin() const { return d_methods; }

    method_iterator method_end() { return d_methods + method_size(); }
    const method_iterator method_end() const { return d_methods + method_size(); }

    std::size_t method_size() const { return d_method_count; }

    const Method *findMethod(llvm::StringRef) const;

private:

    Interface(llvm::StructType *, llvm::ArrayRef<Method>);

    llvm::StructType *d_type = nullptr;

    std::size_t d_method_count = 0;
    Method *d_methods = nullptr;
};

Interface::Interface(llvm::StructType *type, llvm::ArrayRef<Method> methods)
: d_type(type)
, d_method_count(methods.size()) {
    d_methods = std::allocator<Method>().allocate(d_method_count);

    std::accumulate(
        methods.begin(), methods.end(),
        d_methods,
        [](auto plhs, const auto& rhs) -> auto {
            new (plhs++) Method(rhs);
            return plhs;
        });

    std::sort(
        d_methods, d_methods + d_method_count,
        [](const auto& lhs, const auto& rhs) -> auto {
            return lhs.name < rhs.name;
        });
}

const Interface::Method *
Interface::findMethod(llvm::StringRef name) const {
    struct Compare {
        bool operator()(llvm::StringRef lhs, const Method& rhs) const {
            return lhs.compare(rhs.name) < 0;
        }

        bool operator()(const Method& lhs, llvm::StringRef rhs) const {
            return rhs.compare(lhs.name) >= 0;
        }
    };

    auto tmp = std::equal_range(
        d_methods, d_methods + d_method_count,
        name,
        Compare());

    if (tmp.first == tmp.second) {
        return nullptr;
    }

    return tmp.first;
}

class Namespace {
public:

    using Node = std::variant<std::unique_ptr<Class>,
                              std::unique_ptr<Namespace>,
                              std::unique_ptr<Interface> >;

    llvm::StringMap<Node> members;
};

//
int
main(int argc, char **argv) {
  llvm::cl::ParseCommandLineOptions(argc, argv, "llair-link\n");

  llvm::ExitOnError exit_on_err("llosl-link: ");

  auto llvm_context = std::make_unique<llvm::LLVMContext>();
  auto llair_context = std::make_unique<llair::LLAIRContext>(*llvm_context);

  std::vector<std::unique_ptr<llair::Module> > input_modules;

  std::transform(
    input_filenames.begin(), input_filenames.end(),
    std::back_inserter(input_modules),
    [&exit_on_err, &llair_context](auto input_filename) -> std::unique_ptr<llair::Module> {
      auto buffer = exit_on_err(errorOrToExpected(llvm::MemoryBuffer::getFileOrSTDIN(input_filename)));
      auto module = exit_on_err(llair::getBitcodeModule(llvm::MemoryBufferRef(*buffer), *llair_context));
      return module;
    });

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
      [&declared_functions, &defined_functions](auto& input_module) -> void {
          std::for_each(
              input_module->getLLModule()->begin(), input_module->getLLModule()->end(),
              [&declared_functions, &defined_functions](auto& function) -> void {
                  if (function.isDeclarationForLinker()) {
                      declared_functions.push_back(&function);
                  }
                  else if (function.isStrongDefinitionForLinker()) {
                      defined_functions.push_back(&function);
                  }
              });
      });

  std::sort(
      declared_functions.begin(), declared_functions.end(),
      [](auto lhs, auto rhs) -> bool {
          return lhs->getName().compare(rhs->getName()) < 0;
      });

  std::sort(
      defined_functions.begin(), defined_functions.end(),
      [](auto lhs, auto rhs) -> bool {
          return lhs->getName().compare(rhs->getName()) < 0;
      });

  std::vector<llvm::Function *> undefined_functions;

  std::set_difference(
      declared_functions.begin(), declared_functions.end(),
      defined_functions.begin(), defined_functions.end(),
      std::back_inserter(undefined_functions),
      [](auto lhs, auto rhs) -> bool {
          return lhs->getName().compare(rhs->getName()) < 0;
      });

  std::for_each(
      undefined_functions.begin(), undefined_functions.end(),
      [](auto function) -> void {
          auto names = parseClassPathAndMethodName(function);
          if (!names) {
              return;
          }

          auto type = getSelfType(function);
          if (!type) {
              return;
          }

          auto class_name = canonicalizeClassPath(std::get<0>(*names));
          auto method_name = std::get<1>(*names).str();

          std::cerr << class_name << " " << method_name << std::endl;

          (*type)->dump();

          //std::cerr << std::get<0>(*names).str() << " " << std::get<1>(*names).str() << std::endl;
      });

  //
  // Discover implementations:
  // - for each module:
  //   - for each function that is 'defined':
  //     - if it is not part of a class, ignore it
  //     - if the function name is not associated with any interface, ignore it
  //
  //

  auto output_module = std::make_unique<llair::Module>(output_filename, *llair_context);

  std::for_each(
    input_modules.begin(), input_modules.end(),
    [&output_module](auto& input_module) -> void {
      linkModules(output_module.get(), input_module.get());
    });

  // Write it out:
  std::error_code error_code;
  std::unique_ptr<llvm::ToolOutputFile> output_file(
    new llvm::ToolOutputFile(output_filename, error_code, llvm::sys::fs::F_None));

  if (error_code) {
    llvm::errs() << error_code.message();
    return 1;
  }

  llvm::WriteBitcodeToFile(output_module->getLLModule(), output_file->os());
  output_file->keep();

  return 0;
}
