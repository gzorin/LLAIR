//-*-C++-*-

#include <llair/IR/Named.h>
#include <llair/IR/SymbolTable.h>

#include "LLAIRContextImpl.h"

namespace llair {

Named::~Named() {
}

void
Named::setSymbolTable(SymbolTable *symbol_table) {
    if (d_symbol_table == symbol_table) {
        return;
    }

    std::string name = getName().str();
    setName("");

    d_symbol_table = symbol_table;

    setName(name);
}

void
Named::setSymbolTableEntry(SymbolTableEntry *symbol_table_entry) {
    auto& context_impl = LLAIRContextImpl::Get(getContext());

    if (!symbol_table_entry) {
        if (d_has_name) {
            context_impl.names().erase(this);
            d_has_name = false;
        }

        return;
    }

    d_has_name = true;
    context_impl.names()[this] = symbol_table_entry;
}

SymbolTableEntry *
Named::getSymbolTableEntry() const {
    if (!d_has_name) {
        return nullptr;
    }

    auto& context_impl = LLAIRContextImpl::Get(getContext());
    auto it = context_impl.names().find(this);
    assert(it != context_impl.names().end());

    return it->second;
}

void
Named::setName(llvm::StringRef name) {
    if (!d_has_name && name.empty()) {
        return;
    }

    if (getName() == name) {
        return;
    }

    if (!d_symbol_table) {
        destroySymbolTableEntry();

        if (name.empty()) {
            return;
        }

        setSymbolTableEntry(SymbolTableEntry::Create(name, d_allocator));
        getSymbolTableEntry()->setValue(this);

        return;
    }

    if (d_has_name) {
        d_symbol_table->removeSymbolTableEntry(getSymbolTableEntry());
        destroySymbolTableEntry();

        if (name.empty()) {
            return;
        }
    }

    setSymbolTableEntry(d_symbol_table->createSymbolTableEntry(name, this));
}

llvm::StringRef
Named::getName() const {
    if (!d_has_name) {
        return llvm::StringRef("", 0);
    }

    return getSymbolTableEntry()->getKey();
}

void
Named::destroySymbolTableEntry() {
    if (auto symbol_table_entry = getSymbolTableEntry()) {
        if (d_symbol_table) {
            symbol_table_entry->Destroy(d_symbol_table->getAllocator());
        }
        else {
            symbol_table_entry->Destroy(d_allocator);
        }
    }

    setSymbolTableEntry(nullptr);
}

} // End namespace llair