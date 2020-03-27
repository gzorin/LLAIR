//-*-C++-*-
#ifndef LLAIR_IR_NAMED_H
#define LLAIR_IR_NAMED_H

#include <llvm/ADT/StringRef.h>

namespace llvm {
template<typename ValueTy> class StringMapEntry;
}

namespace llair {

class LLAIRContext;
class Named;
class SymbolTable;

using SymbolTableEntry = llvm::StringMapEntry<Named *>;

class Named {
public:

    virtual ~Named();

    bool hasName() const { return d_has_name; }

    void setSymbolTableEntry(SymbolTableEntry *);
    SymbolTableEntry *getSymbolTableEntry() const;

    void setName(llvm::StringRef);
    llvm::StringRef getName() const;

    virtual void dump() const = 0;

private:

    virtual LLAIRContext& getContext() const = 0;

    bool d_has_name = false;

protected:

    void setSymbolTable(SymbolTable *);

    void destroySymbolTableEntry();

    SymbolTable *d_symbol_table = nullptr;
};

} // End namespace llair

#endif
