//-*-C++-*-
#ifndef LLAIR_IR_MODULE_H
#define LLAIR_IR_MODULE_H

#include <llair/IR/LLAIRContext.h>
#include <llair/IR/SymbolTable.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/Twine.h>
#include <llvm/ADT/ilist.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <tuple>

namespace llvm {
class LLVMContext;
class Module;
class ValueSymbolTable;
class raw_ostream;
} // End namespace llvm

namespace llair {
class Class;
class Dispatcher;
class EntryPoint;
class Module;

template<typename T>
struct module_ilist_traits : public llvm::ilist_alloc_traits<T> {

    void addNodeToList(T *);
    void removeNodeFromList(T *);

protected:

    Module *getModule();
};

} // End namespace llair

namespace llvm {

template<>  struct ilist_traits<llair::Class>      : public llair::module_ilist_traits<llair::Class>      {};
template<>  struct ilist_traits<llair::EntryPoint> : public llair::module_ilist_traits<llair::EntryPoint> {};
template<>  struct ilist_traits<llair::Dispatcher> : public llair::module_ilist_traits<llair::Dispatcher> {};

} // End namespace llvm

namespace llair {

class Module {
public:
    using EntryPointListType = llvm::ilist<EntryPoint>;
    using ClassListType      = llvm::ilist<Class>;
    using DispatcherListType = llvm::ilist<Dispatcher>;

    static const Module *Get(const llvm::Module *);
    static Module *      Get(llvm::Module *);

    Module(llvm::StringRef name, LLAIRContext &context);
    Module(std::unique_ptr<llvm::Module> &&module);
    ~Module();

    LLAIRContext &getContext() const { return d_context; }

    llvm::LLVMContext &getLLContext() const { return d_context.getLLContext(); }

    const llvm::Module *          getLLModule() const { return d_llmodule.get(); }
    llvm::Module *                getLLModule() { return d_llmodule.get(); }
    std::unique_ptr<llvm::Module> releaseLLModule();

    //
    struct Version {
        int major = 0, minor = 0, patch = 0;
    };

    //
    void    setVersion(const Version &version);
    Version getVersion() const;

    struct Language {
        std::string name;
        Version     version;
    };

    void     setLanguage(const Language &language);
    Language getLanguage() const;

    //
    const EntryPointListType& getEntryPointList() const { return d_entry_points; };
    EntryPointListType&       getEntryPointList() { return d_entry_points; };

    using const_entry_point_iterator = EntryPointListType::const_iterator;
    using entry_point_iterator       = EntryPointListType::iterator;

    const_entry_point_iterator entry_point_begin() const { return d_entry_points.begin(); };
    entry_point_iterator       entry_point_begin()       { return d_entry_points.begin(); };

    const_entry_point_iterator entry_point_end() const { return d_entry_points.end(); };
    entry_point_iterator       entry_point_end()       { return d_entry_points.end(); };

    static EntryPointListType Module::*getSublistAccess(EntryPoint *) {
        return &Module::d_entry_points;
    }

    EntryPoint *getEntryPoint(llvm::StringRef) const;

    //
    const ClassListType& getClassList() const { return d_classes; }
    ClassListType&       getClassList()       { return d_classes; }

    using const_class_iterator = ClassListType::const_iterator;
    using class_iterator       = ClassListType::iterator;

    const_class_iterator class_begin() const { return d_classes.begin(); };
    class_iterator       class_begin()       { return d_classes.begin(); };

    const_class_iterator class_end() const { return d_classes.end(); };
    class_iterator       class_end()       { return d_classes.end(); };

    static ClassListType Module::*getSublistAccess(Class *) {
        return &Module::d_classes;
    }

    const SymbolTable &getClassSymbolTable() const { return d_class_symbol_table; }
    SymbolTable &getClassSymbolTable() { return d_class_symbol_table; }

    Class *getClass(llvm::StringRef) const;

    Class *getOrLoadClassFromABI(llvm::StringRef);
    std::size_t loadAllClassesFromABI();

    //
    const DispatcherListType& getDispatcherList() const { return d_dispatchers; }
    DispatcherListType&       getDispatcherList()       { return d_dispatchers; }

    using const_dispatcher_iterator = DispatcherListType::const_iterator;
    using dispatcher_iterator       = DispatcherListType::iterator;

    const_dispatcher_iterator dispatcher_begin() const { return d_dispatchers.begin(); };
    dispatcher_iterator       dispatcher_begin()       { return d_dispatchers.begin(); };

    const_dispatcher_iterator dispatcher_end() const { return d_dispatchers.end(); };
    dispatcher_iterator       dispatcher_end()       { return d_dispatchers.end(); };

    static DispatcherListType Module::*getSublistAccess(Dispatcher *) {
        return &Module::d_dispatchers;
    }

    //
    void syncMetadata();

    void print(llvm::raw_ostream&) const;

    void dump() const;

private:

    LLAIRContext &                d_context;
    std::unique_ptr<llvm::Module> d_llmodule;

    llvm::TypedTrackingMDRef<llvm::MDTuple> d_version_md, d_language_md;

    EntryPointListType d_entry_points;

    ClassListType d_classes;
    SymbolTable d_class_symbol_table;

    DispatcherListType d_dispatchers;
};

template<typename T>
Module *
module_ilist_traits<T>::getModule() {
    using ilist_type = llvm::ilist<T>;

    auto offset = (std::size_t)&((Module *)nullptr->*Module::getSublistAccess(static_cast<T *>(nullptr)));
    auto anchor = static_cast<ilist_type *>(this);

    return (Module *)((char *)anchor - offset);
}

} // namespace llair

#endif
