//-*-C++-*-
#ifndef LLAIR_IR_ENTRYPOINT
#define LLAIR_IR_ENTRYPOINT

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/ilist_node.h>
#include <llvm/IR/TrackingMDRef.h>
#include <llvm/Support/Casting.h>

#include <string>
#include <variant>

namespace llvm {
class Argument;
class Function;
class MDNode;
class Metadata;
} // namespace llvm

namespace llair {

class Interface;
class Module;

template<typename T> struct module_ilist_traits;

class EntryPoint : public llvm::ilist_node<EntryPoint> {
public:
    enum EntryPointKind { Vertex = 0, Fragment, Compute };

private:
    EntryPointKind d_kind;

public:
    EntryPointKind getKind() const { return d_kind; }

    static const EntryPoint *Get(const llvm::Function *);
    static EntryPoint *      Get(llvm::Function *);

    virtual ~EntryPoint();

    const Module *getModule() const { return d_module; }
    Module *      getModule() { return d_module; }

    const llvm::Function *getFunction() const;
    llvm::Function *      getFunction();

    void setFunction(llvm::Function *function);

    llvm::StringRef getName() const;

    class Argument {
        EntryPoint *d_parent;

    public:
        const llvm::Argument *getFunctionArgument() const { return d_function_argument; }
        llvm::Argument *      getFunctionArgument() { return d_function_argument; }

        unsigned getArgNo() const;

        llvm::StringRef getName() const;
        void            setName(llvm::StringRef);

        const EntryPoint *getParent() const { return d_parent; }
        EntryPoint *      getParent() { return d_parent; }

        llvm::StringRef getTypeName() const;
        void            setTypeName(llvm::StringRef);

        enum class Interpolation {
            kCenterPerspective,
            kCenterNoPerspective,
            kCentroidPerspective,
            kCentroidNoPerspective,
            kSamplePerspective,
            kSampleNoPerspective,
            kFlat
        };

        enum class Access { kReadOnly, kReadWrite };

        // Buffer:
        struct Buffer {
            unsigned int location0 = 0, location1 = 0;
            Access       access;
            unsigned int type_size = 0, type_align_size = 0;
            llvm::Optional<Interface *> interface_type;
        };

        void          InitDetailsAsBuffer(const Buffer &buffer);
        void          InitDetailsAsBuffer(unsigned int, unsigned int, Access, llvm::Optional<Interface *>);
        bool          AreDetailsBuffer() const { return std::holds_alternative<Buffer>(d_details); }
        const Buffer *GetDetailsAsBuffer() const { return std::get_if<Buffer>(&d_details); }
        Buffer *      GetDetailsAsBuffer() { return std::get_if<Buffer>(&d_details); }

        // IndirectBuffer ("argument buffer"):
        struct IndirectBuffer {
            unsigned int location0 = 0, location1 = 0;
            Access       access;
            unsigned int type_size = 0, type_align_size = 0;
            llvm::Optional<Interface *> interface_type;
        };

        void InitDetailsAsIndirectBuffer(const IndirectBuffer &buffer);
        void InitDetailsAsIndirectBuffer(unsigned int, unsigned int, Access, llvm::Optional<Interface *>);
        bool AreDetailsIndirectBuffer() const {
            return std::holds_alternative<IndirectBuffer>(d_details);
        }
        const IndirectBuffer *GetDetailsAsIndirectBuffer() const {
            return std::get_if<IndirectBuffer>(&d_details);
        }
        IndirectBuffer *GetDetailsAsIndirectBuffer() {
            return std::get_if<IndirectBuffer>(&d_details);
        }

        // Texture:
        struct Texture {
            unsigned int location0 = 0, location1 = 0;
            Access       access;
        };

        void InitDetailsAsTexture(const Texture &texture);
        bool AreDetailsTexture() const { return std::holds_alternative<Texture>(d_details); }
        const Texture *GetDetailsAsTexture() const { return std::get_if<Texture>(&d_details); }
        Texture *      GetDetailsAsTexture() { return std::get_if<Texture>(&d_details); }

        // Vertex stage input:
        struct VertexInput {
            unsigned int location0 = 0, location1 = 0;
            std::string  identifier;
        };

        void InitDetailsAsVertexInput(const VertexInput &vertex_input);
        bool AreDetailsVertexInput() const {
            return std::holds_alternative<VertexInput>(d_details);
        }
        const VertexInput *GetDetailsAsVertexInput() const {
            return std::get_if<VertexInput>(&d_details);
        }
        VertexInput *GetDetailsAsVertexInput() { return std::get_if<VertexInput>(&d_details); }

        // Position:
        struct Position {
            Interpolation interpolation;
        };

        void InitDetailsAsPosition(const Position &position);
        bool AreDetailsPosition() const { return std::holds_alternative<Position>(d_details); }
        const Position *GetDetailsAsPosition() const { return std::get_if<Position>(&d_details); }
        Position *      GetDetailsAsPosition() { return std::get_if<Position>(&d_details); }

        // Fragment stage input:
        struct FragmentInput {
            std::string   identifier;
            Interpolation interpolation;
        };

        void InitDetailsAsFragmentInput(const FragmentInput &fragment_input);
        bool AreDetailsFragmentInput() const {
            return std::holds_alternative<FragmentInput>(d_details);
        }
        const FragmentInput *GetDetailsAsFragmentInput() const {
            return std::get_if<FragmentInput>(&d_details);
        }
        FragmentInput *GetDetailsAsFragmentInput() {
            return std::get_if<FragmentInput>(&d_details);
        }

    private:
        friend class EntryPoint;

        Argument(llvm::Argument *, EntryPoint * = nullptr);
        Argument(llvm::Argument *, llvm::MDTuple *md, EntryPoint *);

        void updateMetadataInEntryPoint();

        llvm::Argument *d_function_argument;

        llvm::TypedTrackingMDRef<llvm::MDTuple> d_md;
        unsigned                                d_md_index = 0;

        llvm::Optional<unsigned> d_name_md_index, d_type_name_md_index, d_interface_type_md_index;

        std::variant<std::monostate, Buffer, IndirectBuffer, Texture, Position, VertexInput,
                     FragmentInput>
            d_details;
    };

    using arg_iterator       = Argument *;
    using const_arg_iterator = const Argument *;

    arg_iterator       arg_begin() { return d_arguments; }
    const arg_iterator arg_begin() const { return d_arguments; }

    arg_iterator       arg_end() { return d_arguments + arg_size(); }
    const arg_iterator arg_end() const { return d_arguments + arg_size(); }

    std::size_t arg_size() const;

    llvm::MDNode *      metadata() { return d_md.get(); }
    const llvm::MDNode *metadata() const { return d_md.get(); }

protected:
    EntryPoint(EntryPointKind, llvm::Function *, Module *);
    EntryPoint(EntryPointKind, llvm::MDNode *, Module *);

    std::size_t d_argument_count = 0;
    Argument *  d_arguments;

    Module *d_module = nullptr;

    llvm::TypedTrackingMDRef<llvm::MDNode>          d_md;
    llvm::TypedTrackingMDRef<llvm::ValueAsMetadata> d_function_md;
    llvm::TypedTrackingMDRef<llvm::MDTuple>         d_arguments_md;

private:
    void setModule(Module *);
    void updateArgumentMetadata(Argument *);

    friend struct module_ilist_traits<EntryPoint>;
};

class VertexEntryPoint : public EntryPoint {
public:
    static VertexEntryPoint *Create(llvm::Function *, unsigned, Module *);

    ~VertexEntryPoint();

    class Output {
        VertexEntryPoint *d_parent;

    public:
        llvm::StringRef getName() const;
        void            setName(llvm::StringRef);

        const VertexEntryPoint *getParent() const { return d_parent; }
        VertexEntryPoint *      getParent() { return d_parent; }

        llvm::StringRef getTypeName() const;
        void            setTypeName(llvm::StringRef);

        enum class Interpolation {
            kCenterPerspective,
            kCenterNoPerspective,
            kCentroidPerspective,
            kCentroidNoPerspective,
            kSamplePerspective,
            kSampleNoPerspective,
            kFlat
        };

        // Position:
        struct Position {};

        void            InitDetailsAsPosition(const Position &position);
        const Position *GetDetailsAsPosition() const { return std::get_if<Position>(&d_details); }
        Position *      GetDetailsAsPosition() { return std::get_if<Position>(&d_details); }

        // Vertex output:
        struct VertexOutput {
            std::string   identifier;
            Interpolation interpolation;
        };

        void                InitDetailsAsVertexOutput(const VertexOutput &vertex_output);
        const VertexOutput *GetDetailsAsVertexOutput() const {
            return std::get_if<VertexOutput>(&d_details);
        }
        VertexOutput *GetDetailsAsVertexOutput() { return std::get_if<VertexOutput>(&d_details); }

    private:
        friend class VertexEntryPoint;

        Output(VertexEntryPoint * = nullptr);
        Output(llvm::MDTuple *md, VertexEntryPoint *);

        void updateMetadataInEntryPoint();

        llvm::TypedTrackingMDRef<llvm::MDTuple> d_md;
        unsigned                                d_md_index = 0;

        llvm::Optional<unsigned> d_name_md_index, d_type_name_md_index;

        std::variant<std::monostate, Position, VertexOutput> d_details;
    };

    using output_iterator       = Output *;
    using const_output_iterator = const Output *;

    output_iterator       output_begin() { return d_outputs; }
    const output_iterator output_begin() const { return d_outputs; }

    output_iterator       output_end() { return d_outputs + output_size(); }
    const output_iterator output_end() const { return d_outputs + output_size(); }

    std::size_t output_size() const;

private:
    friend class Module;
    friend class Output;

    VertexEntryPoint(llvm::Function *, unsigned, Module *);
    VertexEntryPoint(llvm::MDNode *, Module *);

    void updateOutputMetadata(Output *);

    std::size_t d_output_count = 0;
    Output *    d_outputs      = nullptr;

    llvm::TypedTrackingMDRef<llvm::MDTuple> d_outputs_md;
};

class FragmentEntryPoint : public EntryPoint {
public:
    static FragmentEntryPoint *Create(llvm::Function *, bool, unsigned, Module *);

    ~FragmentEntryPoint();

    bool early_fragment_tests_enabled() const { return d_early_fragment_tests_enabled; }

    class Output {
        FragmentEntryPoint *d_parent;

    public:
        llvm::StringRef getName() const;
        void            setName(llvm::StringRef);

        const FragmentEntryPoint *getParent() const { return d_parent; }
        FragmentEntryPoint *      getParent() { return d_parent; }

        llvm::StringRef getTypeName() const;
        void            setTypeName(llvm::StringRef);

        // Depth:
        struct Depth {
            enum Qualifier { kAny, kGreater, kLess };
            Qualifier qualifier;
        };

        void         InitDetailsAsDepth(const Depth &depth);
        const Depth *GetDetailsAsDepth() const { return std::get_if<Depth>(&d_details); }
        Depth *      GetDetailsAsDepth() { return std::get_if<Depth>(&d_details); }

        // Render target:
        struct RenderTarget {
            unsigned int location0 = 0, location1 = 0;
        };

        void                InitDetailsAsRenderTarget(const RenderTarget &render_target);
        const RenderTarget *GetDetailsAsRenderTarget() const {
            return std::get_if<RenderTarget>(&d_details);
        }
        RenderTarget *GetDetailsAsRenderTarget() { return std::get_if<RenderTarget>(&d_details); }

    private:
        friend class FragmentEntryPoint;

        Output(FragmentEntryPoint * = nullptr);
        Output(llvm::MDTuple *md, FragmentEntryPoint *);

        void updateMetadataInEntryPoint();

        llvm::TypedTrackingMDRef<llvm::MDTuple> d_md;
        unsigned                                d_md_index = 0;

        llvm::Optional<unsigned> d_name_md_index, d_type_name_md_index;

        std::variant<std::monostate, Depth, RenderTarget> d_details;
    };

    using output_iterator       = Output *;
    using const_output_iterator = const Output *;

    output_iterator       output_begin() { return d_outputs; }
    const output_iterator output_begin() const { return d_outputs; }

    output_iterator       output_end() { return d_outputs + output_size(); }
    const output_iterator output_end() const { return d_outputs + output_size(); }

    std::size_t output_size() const;

private:
    friend class Module;
    friend class Output;

    FragmentEntryPoint(llvm::Function *, bool, unsigned, Module *);
    FragmentEntryPoint(llvm::MDNode *, Module *);

    void updateOutputMetadata(Output *);

    bool d_early_fragment_tests_enabled = false;

    std::size_t d_output_count = 0;
    Output *    d_outputs      = nullptr;

    llvm::TypedTrackingMDRef<llvm::MDTuple> d_outputs_md;
};

class ComputeEntryPoint : public EntryPoint {
public:
    static ComputeEntryPoint *Create(llvm::Function *, Module *);

private:
    ComputeEntryPoint(llvm::Function *, Module *);
};

} // namespace llair

#endif
