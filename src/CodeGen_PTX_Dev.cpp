#include "CodeGen_PTX_Dev.h"
#include "CSE.h"
#include "CodeGen_GPU_Dev.h"
#include "CodeGen_Internal.h"
#include "CodeGen_LLVM.h"
#include "Debug.h"
#include "ExprUsesVar.h"
#include "IREquality.h"
#include "IRMatch.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "IRPrinter.h"
#include "LLVM_Headers.h"
#include "LLVM_Runtime_Linker.h"
#include "Simplify.h"
#include "Solve.h"
#include "Target.h"

#include <fstream>

// This is declared in NVPTX.h, which is not exported. Ugly, but seems better than
// hardcoding a path to the .h file.
#ifdef WITH_NVPTX
namespace llvm {
FunctionPass *createNVVMReflectPass(const StringMap<int> &Mapping);
}
#endif

namespace Halide {
namespace Internal {

using std::string;
using std::vector;

using namespace llvm;

namespace {

/** A code generator that emits GPU code from a given Halide stmt. */
class CodeGen_PTX_Dev : public CodeGen_LLVM, public CodeGen_GPU_Dev {
public:
    /** Create a PTX device code generator. */
    CodeGen_PTX_Dev(Target host);
    ~CodeGen_PTX_Dev() override;

    void add_kernel(Stmt stmt,
                    const std::string &name,
                    const std::vector<DeviceArgument> &args) override;

    static void test();

    std::vector<char> compile_to_src() override;
    std::string get_current_kernel_name() override;

    void dump() override;

    std::string print_gpu_name(const std::string &name) override;

    std::string api_unique_name() override {
        return "cuda";
    }

protected:
    using CodeGen_LLVM::visit;

    /** (Re)initialize the PTX module. This is separate from compile, since
     * a PTX device module will often have many kernels compiled into it for
     * a single pipeline. */
    /* override */ void init_module() override;

    /** We hold onto the basic block at the start of the device
     * function in order to inject allocas */
    llvm::BasicBlock *entry_block;

    /** Nodes for which we need to override default behavior for the GPU runtime */
    // @{
    void visit(const Call *) override;
    void visit(const For *) override;
    void visit(const Allocate *) override;
    void visit(const Free *) override;
    void visit(const AssertStmt *) override;
    void visit(const Load *) override;
    void visit(const Store *) override;
    void visit(const Atomic *) override;
    void codegen_vector_reduce(const VectorReduce *op, const Expr &init) override;
    // @}

    std::string march() const;
    std::string mcpu() const override;
    std::string mattrs() const override;
    bool use_soft_float_abi() const override;
    int native_vector_bits() const override;
    bool promote_indices() const override {
        return false;
    }

    Type upgrade_type_for_arithmetic(const Type &t) const override {
        return t;
    }
    Type upgrade_type_for_storage(const Type &t) const override;

    /** Map from simt variable names (e.g. foo.__block_id_x) to the llvm
     * ptx intrinsic functions to call to get them. */
    std::string simt_intrinsic(const std::string &name);

    bool supports_atomic_add(const Type &t) const override;
};

CodeGen_PTX_Dev::CodeGen_PTX_Dev(Target host)
    : CodeGen_LLVM(host) {
#if !defined(WITH_NVPTX)
    user_error << "ptx not enabled for this build of Halide.\n";
#endif
    user_assert(llvm_NVPTX_enabled) << "llvm build not configured with nvptx target enabled\n.";

    context = new llvm::LLVMContext();
}

CodeGen_PTX_Dev::~CodeGen_PTX_Dev() {
    // This is required as destroying the context before the module
    // results in a crash. Really, responsibility for destruction
    // should be entirely in the parent class.
    // TODO: Figure out how to better manage the context -- e.g. allow using
    // same one as the host.
    module.reset();
    delete context;
}

Type CodeGen_PTX_Dev::upgrade_type_for_storage(const Type &t) const {
    if (t.element_of() == Float(16)) {
        return t;
    }
    return CodeGen_LLVM::upgrade_type_for_storage(t);
}

void CodeGen_PTX_Dev::add_kernel(Stmt stmt,
                                 const std::string &name,
                                 const std::vector<DeviceArgument> &args) {
    internal_assert(module != nullptr);

    debug(2) << "In CodeGen_PTX_Dev::add_kernel\n";

    // Now deduce the types of the arguments to our function
    vector<llvm::Type *> arg_types(args.size());
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].is_buffer) {
            arg_types[i] = llvm_type_of(UInt(8))->getPointerTo();
        } else {
            arg_types[i] = llvm_type_of(args[i].type);
        }
    }

    // Make our function
    FunctionType *func_t = FunctionType::get(void_t, arg_types, false);
    function = llvm::Function::Create(func_t, llvm::Function::ExternalLinkage, name, module.get());
    set_function_attributes_for_target(function, target);

    // Mark the buffer args as no alias
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].is_buffer) {
            function->addParamAttr(i, Attribute::NoAlias);
        }
    }

    // Make the initial basic block
    entry_block = BasicBlock::Create(*context, "entry", function);
    builder->SetInsertPoint(entry_block);

    // Put the arguments in the symbol table
    vector<string> arg_sym_names;
    {
        size_t i = 0;
        for (auto &fn_arg : function->args()) {

            string arg_sym_name = args[i].name;
            sym_push(arg_sym_name, &fn_arg);
            fn_arg.setName(arg_sym_name);
            arg_sym_names.push_back(arg_sym_name);

            i++;
        }
    }

    // We won't end the entry block yet, because we'll want to add
    // some allocas to it later if there are local allocations. Start
    // a new block to put all the code.
    BasicBlock *body_block = BasicBlock::Create(*context, "body", function);
    builder->SetInsertPoint(body_block);

    debug(1) << "Generating llvm bitcode for kernel...\n";
    // Ok, we have a module, function, context, and a builder
    // pointing at a brand new basic block. We're good to go.
    stmt.accept(this);

    // Now we need to end the function
    builder->CreateRetVoid();

    // Make the entry block point to the body block
    builder->SetInsertPoint(entry_block);
    builder->CreateBr(body_block);

    // Add the nvvm annotation that it is a kernel function.
    llvm::Metadata *md_args[] = {
        llvm::ValueAsMetadata::get(function),
        MDString::get(*context, "kernel"),
        llvm::ValueAsMetadata::get(ConstantInt::get(i32_t, 1))};

    MDNode *md_node = MDNode::get(*context, md_args);

    module->getOrInsertNamedMetadata("nvvm.annotations")->addOperand(md_node);

    // Now verify the function is ok
    verifyFunction(*function);

    // Finally, verify the module is ok
    verifyModule(*module);

    debug(2) << "Done generating llvm bitcode for PTX\n";

    // Clear the symbol table
    for (size_t i = 0; i < arg_sym_names.size(); i++) {
        sym_pop(arg_sym_names[i]);
    }
}

void CodeGen_PTX_Dev::init_module() {
    init_context();

#ifdef WITH_NVPTX
    module = get_initial_module_for_ptx_device(target, context);
#endif
}

void CodeGen_PTX_Dev::visit(const Call *op) {
    if (op->is_intrinsic(Call::gpu_thread_barrier)) {
        // Even though we always insert a __syncthreads equivalent
        // (which has both a device and shared memory fence)
        // check to make sure the intrinsic has the right number of
        // arguments
        internal_assert(op->args.size() == 1) << "gpu_thread_barrier() intrinsic must specify memory fence type.\n";

        const auto *fence_type_ptr = as_const_int(op->args[0]);
        internal_assert(fence_type_ptr) << "gpu_thread_barrier() parameter is not a constant integer.\n";

        llvm::Function *barrier0 = module->getFunction("llvm.nvvm.barrier0");
        internal_assert(barrier0) << "Could not find PTX barrier intrinsic (llvm.nvvm.barrier0)\n";
        builder->CreateCall(barrier0);
        value = ConstantInt::get(i32_t, 0);
    } else {
        CodeGen_LLVM::visit(op);
    }
}

string CodeGen_PTX_Dev::simt_intrinsic(const string &name) {
    if (ends_with(name, ".__thread_id_x")) {
        return "llvm.nvvm.read.ptx.sreg.tid.x";
    } else if (ends_with(name, ".__thread_id_y")) {
        return "llvm.nvvm.read.ptx.sreg.tid.y";
    } else if (ends_with(name, ".__thread_id_z")) {
        return "llvm.nvvm.read.ptx.sreg.tid.z";
    } else if (ends_with(name, ".__thread_id_w")) {
        return "llvm.nvvm.read.ptx.sreg.tid.w";
    } else if (ends_with(name, ".__block_id_x")) {
        return "llvm.nvvm.read.ptx.sreg.ctaid.x";
    } else if (ends_with(name, ".__block_id_y")) {
        return "llvm.nvvm.read.ptx.sreg.ctaid.y";
    } else if (ends_with(name, ".__block_id_z")) {
        return "llvm.nvvm.read.ptx.sreg.ctaid.z";
    } else if (ends_with(name, ".__block_id_w")) {
        return "llvm.nvvm.read.ptx.sreg.ctaid.w";
    }
    internal_error << "simt_intrinsic called on bad variable name\n";
    return "";
}

void CodeGen_PTX_Dev::visit(const For *loop) {
    if (is_gpu_var(loop->name)) {
        Expr simt_idx = Call::make(Int(32), simt_intrinsic(loop->name), std::vector<Expr>(), Call::Extern);
        internal_assert(is_const_zero(loop->min));
        sym_push(loop->name, codegen(simt_idx));
        codegen(loop->body);
        sym_pop(loop->name);
    } else {
        CodeGen_LLVM::visit(loop);
    }
}

void CodeGen_PTX_Dev::visit(const Allocate *alloc) {
    user_assert(!alloc->new_expr.defined()) << "Allocate node inside PTX kernel has custom new expression.\n"
                                            << "(Memoization is not supported inside GPU kernels at present.)\n";
    if (alloc->memory_type == MemoryType::GPUShared) {
        // PTX uses zero in address space 3 as the base address for shared memory
        Value *shared_base = Constant::getNullValue(PointerType::get(i8_t, 3));
        sym_push(alloc->name, shared_base);
    } else {
        debug(2) << "Allocate " << alloc->name << " on device\n";

        string allocation_name = alloc->name;
        debug(3) << "Pushing allocation called " << allocation_name << " onto the symbol table\n";

        // Jump back to the entry and generate an alloca. Note that by
        // jumping back we're rendering any expression we carry back
        // meaningless, so we had better only be dealing with
        // constants here.
        int32_t size = alloc->constant_allocation_size();
        internal_assert(size > 0)
            << "Allocation " << alloc->name << " has a dynamic size. "
            << "This should have been moved to the heap by the "
            << "fuse_gpu_thread_loops lowering pass.\n";

        BasicBlock *here = builder->GetInsertBlock();

        builder->SetInsertPoint(entry_block);
        Value *ptr = builder->CreateAlloca(llvm_type_of(alloc->type), ConstantInt::get(i32_t, size));
        builder->SetInsertPoint(here);
        sym_push(allocation_name, ptr);
    }
    codegen(alloc->body);
}

void CodeGen_PTX_Dev::visit(const Free *f) {
    sym_pop(f->name);
}

void CodeGen_PTX_Dev::visit(const AssertStmt *op) {
    // Discard the error message for now.
    Expr trap = Call::make(Int(32), "halide_ptx_trap", {}, Call::Extern);
    codegen(IfThenElse::make(!op->condition, Evaluate::make(trap)));
}

void CodeGen_PTX_Dev::visit(const Load *op) {

    // Do aligned 4-wide 32-bit loads as a single i128 load.
    const Ramp *r = op->index.as<Ramp>();
    // TODO: lanes >= 4, not lanes == 4
    if (is_const_one(op->predicate) && r && is_const_one(r->stride) && r->lanes == 4 && op->type.bits() == 32) {
        ModulusRemainder align = op->alignment;
        if (align.modulus % 4 == 0 && align.remainder % 4 == 0) {
            Expr index = simplify(r->base / 4);
            Expr equiv = Load::make(UInt(128), op->name, index,
                                    op->image, op->param, const_true(), align / 4);
            equiv = reinterpret(op->type, equiv);
            codegen(equiv);
            return;
        }
    }

    CodeGen_LLVM::visit(op);
}

void CodeGen_PTX_Dev::visit(const Store *op) {
    // Issue atomic store if we are inside an Atomic node.
    if (emit_atomic_stores) {
        user_assert(is_const_one(op->predicate)) << "Atomic update does not support predicated store.\n";
        user_assert(op->value.type().bits() >= 32) << "CUDA: 8-bit or 16-bit atomics are not supported.\n";
    }

    // Do aligned 4-wide 32-bit stores as a single i128 store.
    const Ramp *r = op->index.as<Ramp>();
    // TODO: lanes >= 4, not lanes == 4
    if (is_const_one(op->predicate) && r && is_const_one(r->stride) && r->lanes == 4 && op->value.type().bits() == 32) {
        ModulusRemainder align = op->alignment;
        if (align.modulus % 4 == 0 && align.remainder % 4 == 0) {
            Expr index = simplify(r->base / 4);
            Expr value = reinterpret(UInt(128), op->value);
            Stmt equiv = Store::make(op->name, value, index, op->param, const_true(), align / 4);
            codegen(equiv);
            return;
        }
    }

    CodeGen_LLVM::visit(op);
}

void CodeGen_PTX_Dev::visit(const Atomic *op) {
    // CUDA requires all the threads in a warp to perform the same operations,
    // which means our mutex will lead to deadlock.
    user_assert(op->mutex_name.empty())
        << "The atomic update requires a mutex lock, which is not supported in CUDA.\n";

    // Issue atomic stores.
    ScopedValue<bool> old_emit_atomic_stores(emit_atomic_stores, true);
    CodeGen_LLVM::visit(op);
}

void CodeGen_PTX_Dev::codegen_vector_reduce(const VectorReduce *op, const Expr &init) {
    // Pattern match 8/16-bit dot products

    const int input_lanes = op->value.type().lanes();
    const int factor = input_lanes / op->type.lanes();
    const Mul *mul = op->value.as<Mul>();
    if (op->op == VectorReduce::Add &&
        mul &&
        (factor % 4 == 0) &&
        (op->type.element_of() == Int(32) ||
         op->type.element_of() == UInt(32))) {
        Expr i = init;
        if (!i.defined()) {
            i = cast(mul->type, 0);
        }
        // Try to narrow the multiply args to 8-bit
        Expr a = mul->a, b = mul->b;
        if (op->type.is_uint()) {
            a = lossless_cast(UInt(8, input_lanes), a);
            b = lossless_cast(UInt(8, input_lanes), b);
        } else {
            a = lossless_cast(Int(8, input_lanes), a);
            b = lossless_cast(Int(8, input_lanes), b);
            if (!a.defined()) {
                // try uint
                a = lossless_cast(UInt(8, input_lanes), mul->a);
            }
            if (!b.defined()) {
                b = lossless_cast(UInt(8, input_lanes), mul->b);
            }
        }
        // If we only managed to narrow one of them, try to narrow the
        // other to 16-bit. Swap the args so that it's always 'a'.
        Expr a_orig = mul->a;
        if (a.defined() && !b.defined()) {
            std::swap(a, b);
            a_orig = mul->b;
        }
        if (b.defined() && !a.defined()) {
            // Try 16-bit instead
            a = lossless_cast(UInt(16, input_lanes), a_orig);
            if (!a.defined() && !op->type.is_uint()) {
                a = lossless_cast(Int(16, input_lanes), a_orig);
            }
        }

        if (a.defined() && b.defined()) {
            std::ostringstream ss;
            if (a.type().bits() == 8) {
                ss << "dp4a";
            } else {
                ss << "dp2a";
            }
            if (a.type().is_int()) {
                ss << "_s32";
            } else {
                ss << "_u32";
            }
            if (b.type().is_int()) {
                ss << "_s32";
            } else {
                ss << "_u32";
            }
            const int a_32_bit_words_per_sum = (factor * a.type().bits()) / 32;
            const int b_32_bit_words_per_sum = (factor * b.type().bits()) / 32;
            // Reinterpret a and b as 32-bit values with fewer
            // lanes. If they're aligned dense loads we should just do a
            // different load.
            for (Expr *e : {&a, &b}) {
                int sub_lanes = 32 / e->type().bits();
                const Load *load = e->as<Load>();
                const Ramp *idx = load ? load->index.as<Ramp>() : nullptr;
                if (idx &&
                    is_const_one(idx->stride) &&
                    load->alignment.modulus % sub_lanes == 0 &&
                    load->alignment.remainder % sub_lanes == 0) {
                    Expr new_idx = simplify(idx->base / sub_lanes);
                    int load_lanes = input_lanes / sub_lanes;
                    if (input_lanes > sub_lanes) {
                        new_idx = Ramp::make(new_idx, 1, load_lanes);
                    }
                    *e = Load::make(Int(32, load_lanes),
                                    load->name,
                                    new_idx,
                                    load->image,
                                    load->param,
                                    const_true(load_lanes),
                                    load->alignment / sub_lanes);
                } else {
                    *e = reinterpret(Int(32, input_lanes / sub_lanes), *e);
                }
            }
            string name = ss.str();
            vector<Expr> result;
            for (int l = 0; l < op->type.lanes(); l++) {
                // To compute a single lane of the output, we'll
                // extract the appropriate slice of the args, which
                // have been reinterpreted as 32-bit vectors, then
                // call either dp4a or dp2a the appropriate number of
                // times, and finally sum the result.
                Expr i_slice, a_slice, b_slice;
                if (i.type().is_scalar()) {
                    i_slice = i;
                } else {
                    i_slice = Shuffle::make_extract_element(i, l);
                }
                if (a.type().is_scalar()) {
                    a_slice = a;
                } else {
                    a_slice = Shuffle::make_slice(a, l * a_32_bit_words_per_sum, 1, a_32_bit_words_per_sum);
                }
                if (b.type().is_scalar()) {
                    b_slice = b;
                } else {
                    b_slice = Shuffle::make_slice(b, l * b_32_bit_words_per_sum, 1, b_32_bit_words_per_sum);
                }
                for (int i = 0; i < b_32_bit_words_per_sum; i++) {
                    if (a_slice.type().lanes() == b_slice.type().lanes()) {
                        Expr a_lane, b_lane;
                        if (b_slice.type().is_scalar()) {
                            a_lane = a_slice;
                            b_lane = b_slice;
                        } else {
                            a_lane = Shuffle::make_extract_element(a_slice, i);
                            b_lane = Shuffle::make_extract_element(b_slice, i);
                        }
                        i_slice = Call::make(i_slice.type(), name,
                                             {a_lane, b_lane, i_slice},
                                             Call::PureExtern);
                    } else {
                        internal_assert(a_slice.type().lanes() == 2 * b_slice.type().lanes());
                        Expr a_lane_lo, a_lane_hi, b_lane;
                        if (b_slice.type().is_scalar()) {
                            b_lane = b_slice;
                        } else {
                            b_lane = Shuffle::make_extract_element(b_slice, i);
                        }
                        a_lane_lo = Shuffle::make_extract_element(a_slice, 2 * i);
                        a_lane_hi = Shuffle::make_extract_element(a_slice, 2 * i + 1);
                        i_slice = Call::make(i_slice.type(), name,
                                             {a_lane_lo, a_lane_hi, b_lane, i_slice},
                                             Call::PureExtern);
                    }
                }
                i_slice = simplify(i_slice);
                i_slice = common_subexpression_elimination(i_slice);
                result.push_back(i_slice);
            }
            // Concatenate the per-lane results to get the full vector result
            Expr equiv = Shuffle::make_concat(result);
            equiv.accept(this);
            return;
        }
    }
    CodeGen_LLVM::codegen_vector_reduce(op, init);
}

string CodeGen_PTX_Dev::march() const {
    return "nvptx64";
}

string CodeGen_PTX_Dev::mcpu() const {
    if (target.has_feature(Target::CUDACapability80)) {
        return "sm_80";
    } else if (target.has_feature(Target::CUDACapability75)) {
        return "sm_75";
    } else if (target.has_feature(Target::CUDACapability70)) {
        return "sm_70";
    } else if (target.has_feature(Target::CUDACapability61)) {
        return "sm_61";
    } else if (target.has_feature(Target::CUDACapability50)) {
        return "sm_50";
    } else if (target.has_feature(Target::CUDACapability35)) {
        return "sm_35";
    } else if (target.has_feature(Target::CUDACapability32)) {
        return "sm_32";
    } else if (target.has_feature(Target::CUDACapability30)) {
        return "sm_30";
    } else {
        return "sm_20";
    }
}

string CodeGen_PTX_Dev::mattrs() const {
    if (target.has_feature(Target::CUDACapability80)) {
        return "+ptx70";
    } else if (target.has_feature(Target::CUDACapability70) ||
               target.has_feature(Target::CUDACapability75)) {
        return "+ptx60";
    } else if (target.has_feature(Target::CUDACapability61)) {
        return "+ptx50";
    } else if (target.features_any_of({Target::CUDACapability32,
                                       Target::CUDACapability50})) {
        // Need ptx isa 4.0.
        return "+ptx40";
    } else {
        // Use the default. For llvm 3.5 it's ptx 3.2.
        return "";
    }
}

bool CodeGen_PTX_Dev::use_soft_float_abi() const {
    return false;
}

vector<char> CodeGen_PTX_Dev::compile_to_src() {

#ifdef WITH_NVPTX

    debug(2) << "In CodeGen_PTX_Dev::compile_to_src";

    // DISABLED - hooked in here to force PrintBeforeAll option - seems to be the only way?
    /*char* argv[] = { "llc", "-print-before-all" };*/
    /*int argc = sizeof(argv)/sizeof(char*);*/
    /*cl::ParseCommandLineOptions(argc, argv, "Halide PTX internal compiler\n");*/

    llvm::Triple triple(module->getTargetTriple());

    // Allocate target machine

    std::string err_str;
    const llvm::Target *llvm_target = TargetRegistry::lookupTarget(triple.str(), err_str);
    internal_assert(llvm_target) << err_str << "\n";

    TargetOptions options;
#if LLVM_VERSION < 120
    options.PrintMachineCode = false;
#endif
    options.AllowFPOpFusion = FPOpFusion::Fast;
    options.UnsafeFPMath = true;
    options.NoInfsFPMath = true;
    options.NoNaNsFPMath = true;
    options.HonorSignDependentRoundingFPMathOption = false;
    options.NoZerosInBSS = false;
    options.GuaranteedTailCallOpt = false;
    options.StackAlignmentOverride = 0;

    std::unique_ptr<TargetMachine>
        target_machine(llvm_target->createTargetMachine(triple.str(),
                                                        mcpu(), mattrs(), options,
                                                        llvm::Reloc::PIC_,
                                                        llvm::CodeModel::Small,
                                                        CodeGenOpt::Aggressive));

    internal_assert(target_machine.get()) << "Could not allocate target machine!";

    module->setDataLayout(target_machine->createDataLayout());

    // Set up passes
    llvm::SmallString<8> outstr;
    raw_svector_ostream ostream(outstr);
    ostream.SetUnbuffered();

    legacy::FunctionPassManager function_pass_manager(module.get());
    legacy::PassManager module_pass_manager;

    module_pass_manager.add(createTargetTransformInfoWrapperPass(target_machine->getTargetIRAnalysis()));
    function_pass_manager.add(createTargetTransformInfoWrapperPass(target_machine->getTargetIRAnalysis()));

    // NVidia's libdevice library uses a __nvvm_reflect to choose
    // how to handle denormalized numbers. (The pass replaces calls
    // to __nvvm_reflect with a constant via a map lookup. The inliner
    // pass then resolves these situations to fast code, often a single
    // instruction per decision point.)
    //
    // The default is (more) IEEE like handling. FTZ mode flushes them
    // to zero. (This may only apply to single-precision.)
    //
    // The libdevice documentation covers other options for math accuracy
    // such as replacing division with multiply by the reciprocal and
    // use of fused-multiply-add, but they do not seem to be controlled
    // by this __nvvvm_reflect mechanism and may be flags to earlier compiler
    // passes.
    const int kFTZDenorms = 1;

    // Insert a module flag for the FTZ handling.
    module->addModuleFlag(llvm::Module::Override, "nvvm-reflect-ftz",
                          kFTZDenorms);

    if (kFTZDenorms) {
        for (llvm::Function &fn : *module) {
            fn.addFnAttr("nvptx-f32ftz", "true");
        }
    }

    // At present, we default to *enabling* LLVM loop optimization,
    // unless DisableLLVMLoopOpt is set; we're going to flip this to defaulting
    // to *not* enabling these optimizations (and removing the DisableLLVMLoopOpt feature).
    // See https://github.com/halide/Halide/issues/4113 for more info.
    // (Note that setting EnableLLVMLoopOpt always enables loop opt, regardless
    // of the setting of DisableLLVMLoopOpt.)
    const bool do_loop_opt = !target.has_feature(Target::DisableLLVMLoopOpt) ||
                             target.has_feature(Target::EnableLLVMLoopOpt);

    PassManagerBuilder b;
    b.OptLevel = 3;
    b.Inliner = createFunctionInliningPass(b.OptLevel, 0, false);
    b.LoopVectorize = do_loop_opt;
    b.SLPVectorize = true;
    b.DisableUnrollLoops = !do_loop_opt;

    target_machine->adjustPassManager(b);

    b.populateFunctionPassManager(function_pass_manager);
    b.populateModulePassManager(module_pass_manager);

    // Override default to generate verbose assembly.
    target_machine->Options.MCOptions.AsmVerbose = true;

    // Output string stream

    // Ask the target to add backend passes as necessary.
    bool fail = target_machine->addPassesToEmitFile(module_pass_manager, ostream, nullptr,
                                                    ::llvm::CGFT_AssemblyFile,
                                                    true);
    if (fail) {
        internal_error << "Failed to set up passes to emit PTX source\n";
    }

    // Run optimization passes
    function_pass_manager.doInitialization();
    for (llvm::Module::iterator i = module->begin(); i != module->end(); i++) {
        function_pass_manager.run(*i);
    }
    function_pass_manager.doFinalization();
    module_pass_manager.run(*module);

    if (debug::debug_level() >= 2) {
        dump();
    }
    debug(2) << "Done with CodeGen_PTX_Dev::compile_to_src";

    debug(1) << "PTX kernel:\n"
             << outstr.c_str() << "\n";

    vector<char> buffer(outstr.begin(), outstr.end());

    // Dump the SASS too if the cuda SDK is in the path
    if (debug::debug_level() >= 2) {
        debug(2) << "Compiling PTX to SASS. Will fail if CUDA SDK is not installed (and in the path).\n";

        TemporaryFile ptx(get_current_kernel_name(), ".ptx");
        TemporaryFile sass(get_current_kernel_name(), ".sass");

        std::ofstream f(ptx.pathname());
        f.write(buffer.data(), buffer.size());
        f.close();

        string cmd = "ptxas --gpu-name " + mcpu() + " " + ptx.pathname() + " -o " + sass.pathname();
        if (system(cmd.c_str()) == 0) {
            cmd = "nvdisasm " + sass.pathname();
            int ret = system(cmd.c_str());
            (void)ret;  // Don't care if it fails
        }

        // Note: It works to embed the contents of the .sass file in
        // the buffer instead of the ptx source, and this could help
        // with app startup times. Expose via the target?
        /*
        {
            std::ifstream f(sass.pathname());
            buffer.clear();
            f.seekg(0, std::ios_base::end);
            std::streampos sz = f.tellg();
            buffer.resize(sz);
            f.seekg(0, std::ios_base::beg);
            f.read(buffer.data(), sz);
        }
        */
    }

    // Null-terminate the ptx source
    buffer.push_back(0);
    return buffer;
#else  // WITH_NVPTX
    return vector<char>();
#endif
}

int CodeGen_PTX_Dev::native_vector_bits() const {
    // PTX doesn't really do vectorization. The widest type is a double.
    return 64;
}

string CodeGen_PTX_Dev::get_current_kernel_name() {
    return get_llvm_function_name(function);
}

void CodeGen_PTX_Dev::dump() {
    module->print(dbgs(), nullptr, false, true);
}

std::string CodeGen_PTX_Dev::print_gpu_name(const std::string &name) {
    return name;
}

bool CodeGen_PTX_Dev::supports_atomic_add(const Type &t) const {
    if (t.bits() < 32) {
        // TODO: Half atomics are supported by compute capability 7.x or higher.
        return false;
    }
    if (t.is_int_or_uint()) {
        return true;
    }
    if (t.is_float() && t.bits() == 32) {
        return true;
    }
    if (t.is_float() && t.bits() == 64) {
        // double atomics are supported since CC6.1
        return target.get_cuda_capability_lower_bound() >= 61;
    }
    return false;
}

}  // namespace

CodeGen_GPU_Dev *new_CodeGen_PTX_Dev(const Target &target) {
    return new CodeGen_PTX_Dev(target);
}

}  // namespace Internal
}  // namespace Halide
