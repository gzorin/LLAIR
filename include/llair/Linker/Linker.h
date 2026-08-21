//-*-C++-*-
#ifndef LLAIR_LINKER
#define LLAIR_LINKER

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>

namespace llvm {
class Function;
class Module;
class StructType;
class SwitchInst;
class Type;
} // End namespace llvm

namespace llair {

class Class;
class Interface;
class LLAIRContext;
class Module;

// Type-remap and struct-canonicalization state shared across a batch of links.
// Keys are `llvm::Type *` / struct name, both owned by one `llvm::LLVMContext`;
// a cache is therefore valid only for links within that context. A caller that
// links several modules known to share struct identity reuses one cache so the
// canonicalization survives across `Linker` instances.
class LinkerTypeCache {
public:

    using RemappedTypeMap    = llvm::DenseMap<llvm::Type *, llvm::Type *>;
    using CanonicalStructMap = llvm::StringMap<llvm::StructType *>;

    RemappedTypeMap&    remapped_types()    { return d_remapped_types; }
    CanonicalStructMap& canonical_structs() { return d_canonical_structs; }

private:

    RemappedTypeMap    d_remapped_types;
    CanonicalStructMap d_canonical_structs;
};

void linkModules(Module *, const Module *);
void linkModules(Module *, const Module *, LinkerTypeCache&);
void finalizeInterfaces(Module *, llvm::ArrayRef<Interface *>, std::function<uint32_t(const Class*)>);

class Linker {
public:

    Linker(Module&, LinkerTypeCache&);
    ~Linker();

    void linkModule(const Module *);
    void syncMetadata();

private:

    class TypeMapper;

    std::unique_ptr<TypeMapper> TMap;

    Module& d_dst;
};

} // End namespace llair

#endif
