//===------ PPCGCodeGeneration.cpp - Polly Accelerator Code Generation. ---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Take a scop created by ScopInfo and map it to GPU code using the ppcg
// GPU mapping strategy.
//
//===----------------------------------------------------------------------===//

#include "polly/CodeGen/PPCGCodeGeneration.h"
#include "polly/CodeGen/IslAst.h"
#include "polly/CodeGen/IslNodeBuilder.h"
#include "polly/CodeGen/Utils.h"
#include "polly/DependenceInfo.h"
#include "polly/LinkAllPasses.h"
#include "polly/Options.h"
#include "polly/ScopDetection.h"
#include "polly/ScopInfo.h"
#include "polly/Support/SCEVValidator.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/ScalarEvolutionAliasAnalysis.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <regex>

#include "isl/union_map.h"

extern "C" {
#include "ppcg/cuda.h"
#include "ppcg/gpu.h"
#include "ppcg/gpu_print.h"
#include "ppcg/ppcg.h"
#include "ppcg/schedule.h"
}

#include "llvm/Support/Debug.h"

using namespace polly;
using namespace llvm;

#define DEBUG_TYPE "polly-codegen-ppcg"

static cl::opt<bool> DumpSchedule("polly-acc-dump-schedule",
                                  cl::desc("Dump the computed GPU Schedule"),
                                  cl::Hidden, cl::init(false), cl::ZeroOrMore,
                                  cl::cat(PollyCategory));

static cl::opt<bool>
    DumpCode("polly-acc-dump-code",
             cl::desc("Dump C code describing the GPU mapping"), cl::Hidden,
             cl::init(false), cl::ZeroOrMore, cl::cat(PollyCategory));

static cl::opt<bool> DumpKernelIR("polly-acc-dump-kernel-ir",
                                  cl::desc("Dump the kernel LLVM-IR"),
                                  cl::Hidden, cl::init(false), cl::ZeroOrMore,
                                  cl::cat(PollyCategory));

static cl::opt<bool> DumpKernelASM("polly-acc-dump-kernel-asm",
                                   cl::desc("Dump the kernel assembly code"),
                                   cl::Hidden, cl::init(false), cl::ZeroOrMore,
                                   cl::cat(PollyCategory));

static cl::opt<bool> FastMath("polly-acc-fastmath",
                              cl::desc("Allow unsafe math optimizations"),
                              cl::Hidden, cl::init(false), cl::ZeroOrMore,
                              cl::cat(PollyCategory));
static cl::opt<bool> SharedMemory("polly-acc-use-shared",
                                  cl::desc("Use shared memory"), cl::Hidden,
                                  cl::init(false), cl::ZeroOrMore,
                                  cl::cat(PollyCategory));
static cl::opt<bool> PrivateMemory("polly-acc-use-private",
                                   cl::desc("Use private memory"), cl::Hidden,
                                   cl::init(false), cl::ZeroOrMore,
                                   cl::cat(PollyCategory));

static cl::opt<bool> ManagedMemory("polly-acc-codegen-managed-memory",
                                   cl::desc("Generate Host kernel code assuming"
                                            " that all memory has been"
                                            " declared as managed memory"),
                                   cl::Hidden, cl::init(false), cl::ZeroOrMore,
                                   cl::cat(PollyCategory));

static cl::opt<bool>
    FailOnVerifyModuleFailure("polly-acc-fail-on-verify-module-failure",
                              cl::desc("Fail and generate a backtrace if"
                                       " verifyModule fails on the GPU "
                                       " kernel module."),
                              cl::Hidden, cl::init(false), cl::ZeroOrMore,
                              cl::cat(PollyCategory));

static cl::opt<std::string>
    CudaVersion("polly-acc-cuda-version",
                cl::desc("The CUDA version to compile for"), cl::Hidden,
                cl::init("sm_30"), cl::ZeroOrMore, cl::cat(PollyCategory));

static cl::opt<int>
    MinCompute("polly-acc-mincompute",
               cl::desc("Minimal number of compute statements to run on GPU."),
               cl::Hidden, cl::init(10 * 512 * 512));

/// Used to store information PPCG wants for kills. This information is
/// used by live range reordering.
///
/// @see computeLiveRangeReordering
/// @see GPUNodeBuilder::createPPCGScop
/// @see GPUNodeBuilder::createPPCGProg
struct MustKillsInfo {
  /// Collection of all kill statements that will be sequenced at the end of
  /// PPCGScop->schedule.
  ///
  /// The nodes in `KillsSchedule` will be merged using `isl_schedule_set`
  /// which merges schedules in *arbitrary* order.
  /// (we don't care about the order of the kills anyway).
  isl::schedule KillsSchedule;
  /// Map from kill statement instances to scalars that need to be
  /// killed.
  ///
  /// We currently only derive kill information for phi nodes, as phi nodes
  /// allow us to easily derive kill information. PHI nodes are not alive
  /// outside the scop and can consequently all be "killed". [params] -> {
  /// [Stmt_phantom[] -> ref_phantom[]] -> phi_ref[] }
  isl::union_map TaggedMustKills;

  MustKillsInfo() : KillsSchedule(nullptr), TaggedMustKills(nullptr){};
};

/// Check if SAI's uses are entirely contained within Scop S.
/// If a scalar is used only with a Scop, we are free to kill it, as no data
/// can flow in/out of the value any more.
/// @see computeMustKillsInfo
static bool isScalarUsesContainedInScop(const Scop &S,
                                        const ScopArrayInfo *SAI) {
  assert(SAI->isValueKind() && "this function only deals with scalars."
                               " Dealing with arrays required alias analysis");

  const Region &R = S.getRegion();
  for (User *U : SAI->getBasePtr()->users()) {
    Instruction *I = dyn_cast<Instruction>(U);
    assert(I && "invalid user of scop array info");
    if (!R.contains(I))
      return false;
  }
  return true;
}

/// Compute must-kills needed to enable live range reordering with PPCG.
///
/// @params S The Scop to compute live range reordering information
/// @returns live range reordering information that can be used to setup
/// PPCG.
static MustKillsInfo computeMustKillsInfo(const Scop &S) {
  const isl::space ParamSpace(isl::manage(S.getParamSpace()));
  MustKillsInfo Info;

  // 1. Collect all ScopArrayInfo that satisfy *any* of the criteria:
  //      1.1 phi nodes in scop.
  //      1.2 scalars that are only used within the scop
  SmallVector<isl::id, 4> KillMemIds;
  for (ScopArrayInfo *SAI : S.arrays()) {
    if (SAI->isPHIKind() ||
        (SAI->isValueKind() && isScalarUsesContainedInScop(S, SAI)))
      KillMemIds.push_back(isl::manage(SAI->getBasePtrId()));
  }

  Info.TaggedMustKills = isl::union_map::empty(isl::space(ParamSpace));

  // Initialising KillsSchedule to `isl_set_empty` creates an empty node in the
  // schedule:
  //     - filter: "[control] -> { }"
  // So, we choose to not create this to keep the output a little nicer,
  // at the cost of some code complexity.
  Info.KillsSchedule = nullptr;

  for (isl::id &phiId : KillMemIds) {
    isl::id KillStmtId = isl::id::alloc(
        S.getIslCtx(), std::string("SKill_phantom_").append(phiId.get_name()),
        nullptr);

    // NOTE: construction of tagged_must_kill:
    // 2. We need to construct a map:
    //     [param] -> { [Stmt_phantom[] -> ref_phantom[]] -> phi_ref }
    // To construct this, we use `isl_map_domain_product` on 2 maps`:
    // 2a. StmtToPhi:
    //         [param] -> { Stmt_phantom[] -> phi_ref[] }
    // 2b. PhantomRefToPhi:
    //         [param] -> { ref_phantom[] -> phi_ref[] }
    //
    // Combining these with `isl_map_domain_product` gives us
    // TaggedMustKill:
    //     [param] -> { [Stmt[] -> phantom_ref[]] -> memref[] }

    // 2a. [param] -> { S_2[] -> phi_ref[] }
    isl::map StmtToPhi = isl::map::universe(isl::space(ParamSpace));
    StmtToPhi = StmtToPhi.set_tuple_id(isl::dim::in, isl::id(KillStmtId));
    StmtToPhi = StmtToPhi.set_tuple_id(isl::dim::out, isl::id(phiId));

    isl::id PhantomRefId = isl::id::alloc(
        S.getIslCtx(), std::string("ref_phantom") + phiId.get_name(), nullptr);

    // 2b. [param] -> { phantom_ref[] -> memref[] }
    isl::map PhantomRefToPhi = isl::map::universe(isl::space(ParamSpace));
    PhantomRefToPhi = PhantomRefToPhi.set_tuple_id(isl::dim::in, PhantomRefId);
    PhantomRefToPhi = PhantomRefToPhi.set_tuple_id(isl::dim::out, phiId);

    // 2. [param] -> { [Stmt[] -> phantom_ref[]] -> memref[] }
    isl::map TaggedMustKill = StmtToPhi.domain_product(PhantomRefToPhi);
    Info.TaggedMustKills = Info.TaggedMustKills.unite(TaggedMustKill);

    // 3. Create the kill schedule of the form:
    //     "[param] -> { Stmt_phantom[] }"
    // Then add this to Info.KillsSchedule.
    isl::space KillStmtSpace = ParamSpace;
    KillStmtSpace = KillStmtSpace.set_tuple_id(isl::dim::set, KillStmtId);
    isl::union_set KillStmtDomain = isl::set::universe(KillStmtSpace);

    isl::schedule KillSchedule = isl::schedule::from_domain(KillStmtDomain);
    if (Info.KillsSchedule)
      Info.KillsSchedule = Info.KillsSchedule.set(KillSchedule);
    else
      Info.KillsSchedule = KillSchedule;
  }

  return Info;
}

/// Create the ast expressions for a ScopStmt.
///
/// This function is a callback for to generate the ast expressions for each
/// of the scheduled ScopStmts.
static __isl_give isl_id_to_ast_expr *pollyBuildAstExprForStmt(
    void *StmtT, isl_ast_build *Build,
    isl_multi_pw_aff *(*FunctionIndex)(__isl_take isl_multi_pw_aff *MPA,
                                       isl_id *Id, void *User),
    void *UserIndex,
    isl_ast_expr *(*FunctionExpr)(isl_ast_expr *Expr, isl_id *Id, void *User),
    void *UserExpr) {

  ScopStmt *Stmt = (ScopStmt *)StmtT;

  isl_ctx *Ctx;

  if (!Stmt || !Build)
    return NULL;

  Ctx = isl_ast_build_get_ctx(Build);
  isl_id_to_ast_expr *RefToExpr = isl_id_to_ast_expr_alloc(Ctx, 0);

  for (MemoryAccess *Acc : *Stmt) {
    isl_map *AddrFunc = Acc->getAddressFunction();
    AddrFunc = isl_map_intersect_domain(AddrFunc, Stmt->getDomain());
    isl_id *RefId = Acc->getId();
    isl_pw_multi_aff *PMA = isl_pw_multi_aff_from_map(AddrFunc);
    isl_multi_pw_aff *MPA = isl_multi_pw_aff_from_pw_multi_aff(PMA);
    MPA = isl_multi_pw_aff_coalesce(MPA);
    MPA = FunctionIndex(MPA, RefId, UserIndex);
    isl_ast_expr *Access = isl_ast_build_access_from_multi_pw_aff(Build, MPA);
    Access = FunctionExpr(Access, RefId, UserExpr);
    RefToExpr = isl_id_to_ast_expr_set(RefToExpr, RefId, Access);
  }

  return RefToExpr;
}

/// Given a LLVM Type, compute its size in bytes,
static int computeSizeInBytes(const Type *T) {
  int bytes = T->getPrimitiveSizeInBits() / 8;
  if (bytes == 0)
    bytes = T->getScalarSizeInBits() / 8;
  return bytes;
}

/// Generate code for a GPU specific isl AST.
///
/// The GPUNodeBuilder augments the general existing IslNodeBuilder, which
/// generates code for general-purpose AST nodes, with special functionality
/// for generating GPU specific user nodes.
///
/// @see GPUNodeBuilder::createUser
class GPUNodeBuilder : public IslNodeBuilder {
public:
  GPUNodeBuilder(PollyIRBuilder &Builder, ScopAnnotator &Annotator,
                 const DataLayout &DL, LoopInfo &LI, ScalarEvolution &SE,
                 DominatorTree &DT, Scop &S, BasicBlock *StartBlock,
                 gpu_prog *Prog, GPURuntime Runtime, GPUArch Arch)
      : IslNodeBuilder(Builder, Annotator, DL, LI, SE, DT, S, StartBlock),
        Prog(Prog), Runtime(Runtime), Arch(Arch) {
    getExprBuilder().setIDToSAI(&IDToSAI);
  }

  /// Create after-run-time-check initialization code.
  void initializeAfterRTH();

  /// Finalize the generated scop.
  virtual void finalize();

  /// Track if the full build process was successful.
  ///
  /// This value is set to false, if throughout the build process an error
  /// occurred which prevents us from generating valid GPU code.
  bool BuildSuccessful = true;

  /// The maximal number of loops surrounding a sequential kernel.
  unsigned DeepestSequential = 0;

  /// The maximal number of loops surrounding a parallel kernel.
  unsigned DeepestParallel = 0;

  /// Return the name to set for the ptx_kernel.
  std::string getKernelFuncName(int Kernel_id);

private:
  /// A vector of array base pointers for which a new ScopArrayInfo was created.
  ///
  /// This vector is used to delete the ScopArrayInfo when it is not needed any
  /// more.
  std::vector<Value *> LocalArrays;

  /// A map from ScopArrays to their corresponding device allocations.
  std::map<ScopArrayInfo *, Value *> DeviceAllocations;

  /// The current GPU context.
  Value *GPUContext;

  /// The set of isl_ids allocated in the kernel
  std::vector<isl_id *> KernelIds;

  /// A module containing GPU code.
  ///
  /// This pointer is only set in case we are currently generating GPU code.
  std::unique_ptr<Module> GPUModule;

  /// The GPU program we generate code for.
  gpu_prog *Prog;

  /// The GPU Runtime implementation to use (OpenCL or CUDA).
  GPURuntime Runtime;

  /// The GPU Architecture to target.
  GPUArch Arch;

  /// Class to free isl_ids.
  class IslIdDeleter {
  public:
    void operator()(__isl_take isl_id *Id) { isl_id_free(Id); };
  };

  /// A set containing all isl_ids allocated in a GPU kernel.
  ///
  /// By releasing this set all isl_ids will be freed.
  std::set<std::unique_ptr<isl_id, IslIdDeleter>> KernelIDs;

  IslExprBuilder::IDToScopArrayInfoTy IDToSAI;

  /// Create code for user-defined AST nodes.
  ///
  /// These AST nodes can be of type:
  ///
  ///   - ScopStmt:      A computational statement (TODO)
  ///   - Kernel:        A GPU kernel call (TODO)
  ///   - Data-Transfer: A GPU <-> CPU data-transfer
  ///   - In-kernel synchronization
  ///   - In-kernel memory copy statement
  ///
  /// @param UserStmt The ast node to generate code for.
  virtual void createUser(__isl_take isl_ast_node *UserStmt);

  enum DataDirection { HOST_TO_DEVICE, DEVICE_TO_HOST };

  /// Create code for a data transfer statement
  ///
  /// @param TransferStmt The data transfer statement.
  /// @param Direction The direction in which to transfer data.
  void createDataTransfer(__isl_take isl_ast_node *TransferStmt,
                          enum DataDirection Direction);

  /// Find llvm::Values referenced in GPU kernel.
  ///
  /// @param Kernel The kernel to scan for llvm::Values
  ///
  /// @returns A pair, whose first element contains the set of values
  ///          referenced by the kernel, and whose second element contains the
  ///          set of functions referenced by the kernel. All functions in the
  ///          second set satisfy isValidFunctionInKernel.
  std::pair<SetVector<Value *>, SetVector<Function *>>
  getReferencesInKernel(ppcg_kernel *Kernel);

  /// Compute the sizes of the execution grid for a given kernel.
  ///
  /// @param Kernel The kernel to compute grid sizes for.
  ///
  /// @returns A tuple with grid sizes for X and Y dimension
  std::tuple<Value *, Value *> getGridSizes(ppcg_kernel *Kernel);

  /// Creates a array that can be sent to the kernel on the device using a
  /// host pointer. This is required for managed memory, when we directly send
  /// host pointers to the device.
  /// \note
  /// This is to be used only with managed memory
  Value *getOrCreateManagedDeviceArray(gpu_array_info *Array,
                                       ScopArrayInfo *ArrayInfo);

  /// Compute the sizes of the thread blocks for a given kernel.
  ///
  /// @param Kernel The kernel to compute thread block sizes for.
  ///
  /// @returns A tuple with thread block sizes for X, Y, and Z dimensions.
  std::tuple<Value *, Value *, Value *> getBlockSizes(ppcg_kernel *Kernel);

  /// Store a specific kernel launch parameter in the array of kernel launch
  /// parameters.
  ///
  /// @param Parameters The list of parameters in which to store.
  /// @param Param      The kernel launch parameter to store.
  /// @param Index      The index in the parameter list, at which to store the
  ///                   parameter.
  void insertStoreParameter(Instruction *Parameters, Instruction *Param,
                            int Index);

  /// Create kernel launch parameters.
  ///
  /// @param Kernel        The kernel to create parameters for.
  /// @param F             The kernel function that has been created.
  /// @param SubtreeValues The set of llvm::Values referenced by this kernel.
  ///
  /// @returns A stack allocated array with pointers to the parameter
  ///          values that are passed to the kernel.
  Value *createLaunchParameters(ppcg_kernel *Kernel, Function *F,
                                SetVector<Value *> SubtreeValues);

  /// Create declarations for kernel variable.
  ///
  /// This includes shared memory declarations.
  ///
  /// @param Kernel        The kernel definition to create variables for.
  /// @param FN            The function into which to generate the variables.
  void createKernelVariables(ppcg_kernel *Kernel, Function *FN);

  /// Add CUDA annotations to module.
  ///
  /// Add a set of CUDA annotations that declares the maximal block dimensions
  /// that will be used to execute the CUDA kernel. This allows the NVIDIA
  /// PTX compiler to bound the number of allocated registers to ensure the
  /// resulting kernel is known to run with up to as many block dimensions
  /// as specified here.
  ///
  /// @param M         The module to add the annotations to.
  /// @param BlockDimX The size of block dimension X.
  /// @param BlockDimY The size of block dimension Y.
  /// @param BlockDimZ The size of block dimension Z.
  void addCUDAAnnotations(Module *M, Value *BlockDimX, Value *BlockDimY,
                          Value *BlockDimZ);

  /// Create GPU kernel.
  ///
  /// Code generate the kernel described by @p KernelStmt.
  ///
  /// @param KernelStmt The ast node to generate kernel code for.
  void createKernel(__isl_take isl_ast_node *KernelStmt);

  /// Generate code that computes the size of an array.
  ///
  /// @param Array The array for which to compute a size.
  Value *getArraySize(gpu_array_info *Array);

  /// Generate code to compute the minimal offset at which an array is accessed.
  ///
  /// The offset of an array is the minimal array location accessed in a scop.
  ///
  /// Example:
  ///
  ///   for (long i = 0; i < 100; i++)
  ///     A[i + 42] += ...
  ///
  ///   getArrayOffset(A) results in 42.
  ///
  /// @param Array The array for which to compute the offset.
  /// @returns An llvm::Value that contains the offset of the array.
  Value *getArrayOffset(gpu_array_info *Array);

  /// Prepare the kernel arguments for kernel code generation
  ///
  /// @param Kernel The kernel to generate code for.
  /// @param FN     The function created for the kernel.
  void prepareKernelArguments(ppcg_kernel *Kernel, Function *FN);

  /// Create kernel function.
  ///
  /// Create a kernel function located in a newly created module that can serve
  /// as target for device code generation. Set the Builder to point to the
  /// start block of this newly created function.
  ///
  /// @param Kernel The kernel to generate code for.
  /// @param SubtreeValues The set of llvm::Values referenced by this kernel.
  /// @param SubtreeFunctions The set of llvm::Functions referenced by this
  ///                         kernel.
  void createKernelFunction(ppcg_kernel *Kernel,
                            SetVector<Value *> &SubtreeValues,
                            SetVector<Function *> &SubtreeFunctions);

  /// Create the declaration of a kernel function.
  ///
  /// The kernel function takes as arguments:
  ///
  ///   - One i8 pointer for each external array reference used in the kernel.
  ///   - Host iterators
  ///   - Parameters
  ///   - Other LLVM Value references (TODO)
  ///
  /// @param Kernel The kernel to generate the function declaration for.
  /// @param SubtreeValues The set of llvm::Values referenced by this kernel.
  ///
  /// @returns The newly declared function.
  Function *createKernelFunctionDecl(ppcg_kernel *Kernel,
                                     SetVector<Value *> &SubtreeValues);

  /// Insert intrinsic functions to obtain thread and block ids.
  ///
  /// @param The kernel to generate the intrinsic functions for.
  void insertKernelIntrinsics(ppcg_kernel *Kernel);

  /// Setup the creation of functions referenced by the GPU kernel.
  ///
  /// 1. Create new function declarations in GPUModule which are the same as
  /// SubtreeFunctions.
  ///
  /// 2. Populate IslNodeBuilder::ValueMap with mappings from
  /// old functions (that come from the original module) to new functions
  /// (that are created within GPUModule). That way, we generate references
  /// to the correct function (in GPUModule) in BlockGenerator.
  ///
  /// @see IslNodeBuilder::ValueMap
  /// @see BlockGenerator::GlobalMap
  /// @see BlockGenerator::getNewValue
  /// @see GPUNodeBuilder::getReferencesInKernel.
  ///
  /// @param SubtreeFunctions The set of llvm::Functions referenced by
  ///                         this kernel.
  void setupKernelSubtreeFunctions(SetVector<Function *> SubtreeFunctions);

  /// Create a global-to-shared or shared-to-global copy statement.
  ///
  /// @param CopyStmt The copy statement to generate code for
  void createKernelCopy(ppcg_kernel_stmt *CopyStmt);

  /// Create code for a ScopStmt called in @p Expr.
  ///
  /// @param Expr The expression containing the call.
  /// @param KernelStmt The kernel statement referenced in the call.
  void createScopStmt(isl_ast_expr *Expr, ppcg_kernel_stmt *KernelStmt);

  /// Create an in-kernel synchronization call.
  void createKernelSync();

  /// Create a PTX assembly string for the current GPU kernel.
  ///
  /// @returns A string containing the corresponding PTX assembly code.
  std::string createKernelASM();

  /// Create a SPIR string for the current GPU kernel.
  ///
  /// @returns A string containing the corresponding  SPIR code.
  std::string createKernelSPIR(std::string IR);

  /// Remove references from the dominator tree to the kernel function @p F.
  ///
  /// @param F The function to remove references to.
  void clearDominators(Function *F);

  /// Remove references from scalar evolution to the kernel function @p F.
  ///
  /// @param F The function to remove references to.
  void clearScalarEvolution(Function *F);

  /// Remove references from loop info to the kernel function @p F.
  ///
  /// @param F The function to remove references to.
  void clearLoops(Function *F);

  /// Finalize the generation of the kernel function.
  ///
  /// Free the LLVM-IR module corresponding to the kernel and -- if requested --
  /// dump its IR to stderr.
  ///
  /// @returns The Assembly string of the kernel.
  std::string finalizeKernelFunction();

  /// Finalize the generation of the kernel arguments.
  ///
  /// This function ensures that not-read-only scalars used in a kernel are
  /// stored back to the global memory location they are backed with before
  /// the kernel terminates.
  ///
  /// @params Kernel The kernel to finalize kernel arguments for.
  void finalizeKernelArguments(ppcg_kernel *Kernel);

  /// Create code that allocates memory to store arrays on device.
  void allocateDeviceArrays();

  /// Free all allocated device arrays.
  void freeDeviceArrays();

  /// Create a call to initialize the GPU context.
  ///
  /// @returns A pointer to the newly initialized context.
  Value *createCallInitContext();

  /// Create a call to get the device pointer for a kernel allocation.
  ///
  /// @param Allocation The Polly GPU allocation
  ///
  /// @returns The device parameter corresponding to this allocation.
  Value *createCallGetDevicePtr(Value *Allocation);

  /// Create a call to free the GPU context.
  ///
  /// @param Context A pointer to an initialized GPU context.
  void createCallFreeContext(Value *Context);

  /// Create a call to allocate memory on the device.
  ///
  /// @param Size The size of memory to allocate
  ///
  /// @returns A pointer that identifies this allocation.
  Value *createCallAllocateMemoryForDevice(Value *Size);

  /// Create a call to free a device array.
  ///
  /// @param Array The device array to free.
  void createCallFreeDeviceMemory(Value *Array);

  /// Create a call to copy data from host to device.
  ///
  /// @param HostPtr A pointer to the host data that should be copied.
  /// @param DevicePtr A device pointer specifying the location to copy to.
  void createCallCopyFromHostToDevice(Value *HostPtr, Value *DevicePtr,
                                      Value *Size);

  /// Create a call to copy data from device to host.
  ///
  /// @param DevicePtr A pointer to the device data that should be copied.
  /// @param HostPtr A host pointer specifying the location to copy to.
  void createCallCopyFromDeviceToHost(Value *DevicePtr, Value *HostPtr,
                                      Value *Size);

  /// Create a call to synchronize Host & Device.
  /// \note
  /// This is to be used only with managed memory.
  void createCallSynchronizeDevice();

  /// Create a call to get a kernel from an assembly string.
  ///
  /// @param Buffer The string describing the kernel.
  /// @param Entry  The name of the kernel function to call.
  ///
  /// @returns A pointer to a kernel object
  Value *createCallGetKernel(Value *Buffer, Value *Entry);

  /// Create a call to free a GPU kernel.
  ///
  /// @param GPUKernel THe kernel to free.
  void createCallFreeKernel(Value *GPUKernel);

  /// Create a call to launch a GPU kernel.
  ///
  /// @param GPUKernel  The kernel to launch.
  /// @param GridDimX   The size of the first grid dimension.
  /// @param GridDimY   The size of the second grid dimension.
  /// @param GridBlockX The size of the first block dimension.
  /// @param GridBlockY The size of the second block dimension.
  /// @param GridBlockZ The size of the third block dimension.
  /// @param Parameters A pointer to an array that contains itself pointers to
  ///                   the parameter values passed for each kernel argument.
  void createCallLaunchKernel(Value *GPUKernel, Value *GridDimX,
                              Value *GridDimY, Value *BlockDimX,
                              Value *BlockDimY, Value *BlockDimZ,
                              Value *Parameters);
};

std::string GPUNodeBuilder::getKernelFuncName(int Kernel_id) {
  return "FUNC_" + S.getFunction().getName().str() + "_KERNEL_" +
         std::to_string(Kernel_id);
}

void GPUNodeBuilder::initializeAfterRTH() {
  BasicBlock *NewBB = SplitBlock(Builder.GetInsertBlock(),
                                 &*Builder.GetInsertPoint(), &DT, &LI);
  NewBB->setName("polly.acc.initialize");
  Builder.SetInsertPoint(&NewBB->front());

  GPUContext = createCallInitContext();

  if (!ManagedMemory)
    allocateDeviceArrays();
}

void GPUNodeBuilder::finalize() {
  if (!ManagedMemory)
    freeDeviceArrays();

  createCallFreeContext(GPUContext);
  IslNodeBuilder::finalize();
}

void GPUNodeBuilder::allocateDeviceArrays() {
  assert(!ManagedMemory && "Managed memory will directly send host pointers "
                           "to the kernel. There is no need for device arrays");
  isl_ast_build *Build = isl_ast_build_from_context(S.getContext());

  for (int i = 0; i < Prog->n_array; ++i) {
    gpu_array_info *Array = &Prog->array[i];
    auto *ScopArray = (ScopArrayInfo *)Array->user;
    std::string DevArrayName("p_dev_array_");
    DevArrayName.append(Array->name);

    Value *ArraySize = getArraySize(Array);
    Value *Offset = getArrayOffset(Array);
    if (Offset)
      ArraySize = Builder.CreateSub(
          ArraySize,
          Builder.CreateMul(Offset,
                            Builder.getInt64(ScopArray->getElemSizeInBytes())));
    Value *DevArray = createCallAllocateMemoryForDevice(ArraySize);
    DevArray->setName(DevArrayName);
    DeviceAllocations[ScopArray] = DevArray;
  }

  isl_ast_build_free(Build);
}

void GPUNodeBuilder::addCUDAAnnotations(Module *M, Value *BlockDimX,
                                        Value *BlockDimY, Value *BlockDimZ) {
  auto AnnotationNode = M->getOrInsertNamedMetadata("nvvm.annotations");

  for (auto &F : *M) {
    if (F.getCallingConv() != CallingConv::PTX_Kernel)
      continue;

    Value *V[] = {BlockDimX, BlockDimY, BlockDimZ};

    Metadata *Elements[] = {
        ValueAsMetadata::get(&F),   MDString::get(M->getContext(), "maxntidx"),
        ValueAsMetadata::get(V[0]), MDString::get(M->getContext(), "maxntidy"),
        ValueAsMetadata::get(V[1]), MDString::get(M->getContext(), "maxntidz"),
        ValueAsMetadata::get(V[2]),
    };
    MDNode *Node = MDNode::get(M->getContext(), Elements);
    AnnotationNode->addOperand(Node);
  }
}

void GPUNodeBuilder::freeDeviceArrays() {
  assert(!ManagedMemory && "Managed memory does not use device arrays");
  for (auto &Array : DeviceAllocations)
    createCallFreeDeviceMemory(Array.second);
}

Value *GPUNodeBuilder::createCallGetKernel(Value *Buffer, Value *Entry) {
  const char *Name = "polly_getKernel";
  Module *M = Builder.GetInsertBlock()->getParent()->getParent();
  Function *F = M->getFunction(Name);

  // If F is not available, declare it.
  if (!F) {
    GlobalValue::LinkageTypes Linkage = Function::ExternalLinkage;
    std::vector<Type *> Args;
    Args.push_back(Builder.getInt8PtrTy());
    Args.push_back(Builder.getInt8PtrTy());
    FunctionType *Ty = FunctionType::get(Builder.getInt8PtrTy(), Args, false);
    F = Function::Create(Ty, Linkage, Name, M);
  }

  return Builder.CreateCall(F, {Buffer, Entry});
}

Value *GPUNodeBuilder::createCallGetDevicePtr(Value *Allocation) {
  const char *Name = "polly_getDevicePtr";
  Module *M = Builder.GetInsertBlock()->getParent()->getParent();
  Function *F = M->getFunction(Name);

  // If F is not available, declare it.
  if (!F) {
    GlobalValue::LinkageTypes Linkage = Function::ExternalLinkage;
    std::vector<Type *> Args;
    Args.push_back(Builder.getInt8PtrTy());
    FunctionType *Ty = FunctionType::get(Builder.getInt8PtrTy(), Args, false);
    F = Function::Create(Ty, Linkage, Name, M);
  }

  return Builder.CreateCall(F, {Allocation});
}

void GPUNodeBuilder::createCallLaunchKernel(Value *GPUKernel, Value *GridDimX,
                                            Value *GridDimY, Value *BlockDimX,
                                            Value *BlockDimY, Value *BlockDimZ,
                                            Value *Parameters) {
  const char *Name = "polly_launchKernel";
  Module *M = Builder.GetInsertBlock()->getParent()->getParent();
  Function *F = M->getFunction(Name);

  // If F is not available, declare it.
  if (!F) {
    GlobalValue::LinkageTypes Linkage = Function::ExternalLinkage;
    std::vector<Type *> Args;
    Args.push_back(Builder.getInt8PtrTy());
    Args.push_back(Builder.getInt32Ty());
    Args.push_back(Builder.getInt32Ty());
    Args.push_back(Builder.getInt32Ty());
    Args.push_back(Builder.getInt32Ty());
    Args.push_back(Builder.getInt32Ty());
    Args.push_back(Builder.getInt8PtrTy());
    FunctionType *Ty = FunctionType::get(Builder.getVoidTy(), Args, false);
    F = Function::Create(Ty, Linkage, Name, M);
  }

  Builder.CreateCall(F, {GPUKernel, GridDimX, GridDimY, BlockDimX, BlockDimY,
                         BlockDimZ, Parameters});
}

void GPUNodeBuilder::createCallFreeKernel(Value *GPUKernel) {
  const char *Name = "polly_freeKernel";
  Module *M = Builder.GetInsertBlock()->getParent()->getParent();
  Function *F = M->getFunction(Name);

  // If F is not available, declare it.
  if (!F) {
    GlobalValue::LinkageTypes Linkage = Function::ExternalLinkage;
    std::vector<Type *> Args;
    Args.push_back(Builder.getInt8PtrTy());
    FunctionType *Ty = FunctionType::get(Builder.getVoidTy(), Args, false);
    F = Function::Create(Ty, Linkage, Name, M);
  }

  Builder.CreateCall(F, {GPUKernel});
}

void GPUNodeBuilder::createCallFreeDeviceMemory(Value *Array) {
  assert(!ManagedMemory && "Managed memory does not allocate or free memory "
                           "for device");
  const char *Name = "polly_freeDeviceMemory";
  Module *M = Builder.GetInsertBlock()->getParent()->getParent();
  Function *F = M->getFunction(Name);

  // If F is not available, declare it.
  if (!F) {
    GlobalValue::LinkageTypes Linkage = Function::ExternalLinkage;
    std::vector<Type *> Args;
    Args.push_back(Builder.getInt8PtrTy());
    FunctionType *Ty = FunctionType::get(Builder.getVoidTy(), Args, false);
    F = Function::Create(Ty, Linkage, Name, M);
  }

  Builder.CreateCall(F, {Array});
}

Value *GPUNodeBuilder::createCallAllocateMemoryForDevice(Value *Size) {
  assert(!ManagedMemory && "Managed memory does not allocate or free memory "
                           "for device");
  const char *Name = "polly_allocateMemoryForDevice";
  Module *M = Builder.GetInsertBlock()->getParent()->getParent();
  Function *F = M->getFunction(Name);

  // If F is not available, declare it.
  if (!F) {
    GlobalValue::LinkageTypes Linkage = Function::ExternalLinkage;
    std::vector<Type *> Args;
    Args.push_back(Builder.getInt64Ty());
    FunctionType *Ty = FunctionType::get(Builder.getInt8PtrTy(), Args, false);
    F = Function::Create(Ty, Linkage, Name, M);
  }

  return Builder.CreateCall(F, {Size});
}

void GPUNodeBuilder::createCallCopyFromHostToDevice(Value *HostData,
                                                    Value *DeviceData,
                                                    Value *Size) {
  assert(!ManagedMemory && "Managed memory does not transfer memory between "
                           "device and host");
  const char *Name = "polly_copyFromHostToDevice";
  Module *M = Builder.GetInsertBlock()->getParent()->getParent();
  Function *F = M->getFunction(Name);

  // If F is not available, declare it.
  if (!F) {
    GlobalValue::LinkageTypes Linkage = Function::ExternalLinkage;
    std::vector<Type *> Args;
    Args.push_back(Builder.getInt8PtrTy());
    Args.push_back(Builder.getInt8PtrTy());
    Args.push_back(Builder.getInt64Ty());
    FunctionType *Ty = FunctionType::get(Builder.getVoidTy(), Args, false);
    F = Function::Create(Ty, Linkage, Name, M);
  }

  Builder.CreateCall(F, {HostData, DeviceData, Size});
}

void GPUNodeBuilder::createCallCopyFromDeviceToHost(Value *DeviceData,
                                                    Value *HostData,
                                                    Value *Size) {
  assert(!ManagedMemory && "Managed memory does not transfer memory between "
                           "device and host");
  const char *Name = "polly_copyFromDeviceToHost";
  Module *M = Builder.GetInsertBlock()->getParent()->getParent();
  Function *F = M->getFunction(Name);

  // If F is not available, declare it.
  if (!F) {
    GlobalValue::LinkageTypes Linkage = Function::ExternalLinkage;
    std::vector<Type *> Args;
    Args.push_back(Builder.getInt8PtrTy());
    Args.push_back(Builder.getInt8PtrTy());
    Args.push_back(Builder.getInt64Ty());
    FunctionType *Ty = FunctionType::get(Builder.getVoidTy(), Args, false);
    F = Function::Create(Ty, Linkage, Name, M);
  }

  Builder.CreateCall(F, {DeviceData, HostData, Size});
}

void GPUNodeBuilder::createCallSynchronizeDevice() {
  assert(ManagedMemory && "explicit synchronization is only necessary for "
                          "managed memory");
  const char *Name = "polly_synchronizeDevice";
  Module *M = Builder.GetInsertBlock()->getParent()->getParent();
  Function *F = M->getFunction(Name);

  // If F is not available, declare it.
  if (!F) {
    GlobalValue::LinkageTypes Linkage = Function::ExternalLinkage;
    FunctionType *Ty = FunctionType::get(Builder.getVoidTy(), false);
    F = Function::Create(Ty, Linkage, Name, M);
  }

  Builder.CreateCall(F);
}

Value *GPUNodeBuilder::createCallInitContext() {
  const char *Name;

  switch (Runtime) {
  case GPURuntime::CUDA:
    Name = "polly_initContextCUDA";
    break;
  case GPURuntime::OpenCL:
    Name = "polly_initContextCL";
    break;
  }

  Module *M = Builder.GetInsertBlock()->getParent()->getParent();
  Function *F = M->getFunction(Name);

  // If F is not available, declare it.
  if (!F) {
    GlobalValue::LinkageTypes Linkage = Function::ExternalLinkage;
    std::vector<Type *> Args;
    FunctionType *Ty = FunctionType::get(Builder.getInt8PtrTy(), Args, false);
    F = Function::Create(Ty, Linkage, Name, M);
  }

  return Builder.CreateCall(F, {});
}

void GPUNodeBuilder::createCallFreeContext(Value *Context) {
  const char *Name = "polly_freeContext";
  Module *M = Builder.GetInsertBlock()->getParent()->getParent();
  Function *F = M->getFunction(Name);

  // If F is not available, declare it.
  if (!F) {
    GlobalValue::LinkageTypes Linkage = Function::ExternalLinkage;
    std::vector<Type *> Args;
    Args.push_back(Builder.getInt8PtrTy());
    FunctionType *Ty = FunctionType::get(Builder.getVoidTy(), Args, false);
    F = Function::Create(Ty, Linkage, Name, M);
  }

  Builder.CreateCall(F, {Context});
}

/// Check if one string is a prefix of another.
///
/// @param String The string in which to look for the prefix.
/// @param Prefix The prefix to look for.
static bool isPrefix(std::string String, std::string Prefix) {
  return String.find(Prefix) == 0;
}

Value *GPUNodeBuilder::getArraySize(gpu_array_info *Array) {
  isl_ast_build *Build = isl_ast_build_from_context(S.getContext());
  Value *ArraySize = ConstantInt::get(Builder.getInt64Ty(), Array->size);

  if (!gpu_array_is_scalar(Array)) {
    auto OffsetDimZero = isl_pw_aff_copy(Array->bound[0]);
    isl_ast_expr *Res = isl_ast_build_expr_from_pw_aff(Build, OffsetDimZero);

    for (unsigned int i = 1; i < Array->n_index; i++) {
      isl_pw_aff *Bound_I = isl_pw_aff_copy(Array->bound[i]);
      isl_ast_expr *Expr = isl_ast_build_expr_from_pw_aff(Build, Bound_I);
      Res = isl_ast_expr_mul(Res, Expr);
    }

    Value *NumElements = ExprBuilder.create(Res);
    if (NumElements->getType() != ArraySize->getType())
      NumElements = Builder.CreateSExt(NumElements, ArraySize->getType());
    ArraySize = Builder.CreateMul(ArraySize, NumElements);
  }
  isl_ast_build_free(Build);
  return ArraySize;
}

Value *GPUNodeBuilder::getArrayOffset(gpu_array_info *Array) {
  if (gpu_array_is_scalar(Array))
    return nullptr;

  isl_ast_build *Build = isl_ast_build_from_context(S.getContext());

  isl_set *Min = isl_set_lexmin(isl_set_copy(Array->extent));

  isl_set *ZeroSet = isl_set_universe(isl_set_get_space(Min));

  for (long i = 0; i < isl_set_dim(Min, isl_dim_set); i++)
    ZeroSet = isl_set_fix_si(ZeroSet, isl_dim_set, i, 0);

  if (isl_set_is_subset(Min, ZeroSet)) {
    isl_set_free(Min);
    isl_set_free(ZeroSet);
    isl_ast_build_free(Build);
    return nullptr;
  }
  isl_set_free(ZeroSet);

  isl_ast_expr *Result =
      isl_ast_expr_from_val(isl_val_int_from_si(isl_set_get_ctx(Min), 0));

  for (long i = 0; i < isl_set_dim(Min, isl_dim_set); i++) {
    if (i > 0) {
      isl_pw_aff *Bound_I = isl_pw_aff_copy(Array->bound[i - 1]);
      isl_ast_expr *BExpr = isl_ast_build_expr_from_pw_aff(Build, Bound_I);
      Result = isl_ast_expr_mul(Result, BExpr);
    }
    isl_pw_aff *DimMin = isl_set_dim_min(isl_set_copy(Min), i);
    isl_ast_expr *MExpr = isl_ast_build_expr_from_pw_aff(Build, DimMin);
    Result = isl_ast_expr_add(Result, MExpr);
  }

  Value *ResultValue = ExprBuilder.create(Result);
  isl_set_free(Min);
  isl_ast_build_free(Build);

  return ResultValue;
}

Value *GPUNodeBuilder::getOrCreateManagedDeviceArray(gpu_array_info *Array,
                                                     ScopArrayInfo *ArrayInfo) {

  assert(ManagedMemory && "Only used when you wish to get a host "
                          "pointer for sending data to the kernel, "
                          "with managed memory");
  std::map<ScopArrayInfo *, Value *>::iterator it;
  if ((it = DeviceAllocations.find(ArrayInfo)) != DeviceAllocations.end()) {
    return it->second;
  } else {
    Value *HostPtr;

    if (gpu_array_is_scalar(Array))
      HostPtr = BlockGen.getOrCreateAlloca(ArrayInfo);
    else
      HostPtr = ArrayInfo->getBasePtr();

    Value *Offset = getArrayOffset(Array);
    if (Offset) {
      HostPtr = Builder.CreatePointerCast(
          HostPtr, ArrayInfo->getElementType()->getPointerTo());
      HostPtr = Builder.CreateGEP(HostPtr, Offset);
    }

    HostPtr = Builder.CreatePointerCast(HostPtr, Builder.getInt8PtrTy());
    DeviceAllocations[ArrayInfo] = HostPtr;
    return HostPtr;
  }
}

void GPUNodeBuilder::createDataTransfer(__isl_take isl_ast_node *TransferStmt,
                                        enum DataDirection Direction) {
  assert(!ManagedMemory && "Managed memory needs no data transfers");
  isl_ast_expr *Expr = isl_ast_node_user_get_expr(TransferStmt);
  isl_ast_expr *Arg = isl_ast_expr_get_op_arg(Expr, 0);
  isl_id *Id = isl_ast_expr_get_id(Arg);
  auto Array = (gpu_array_info *)isl_id_get_user(Id);
  auto ScopArray = (ScopArrayInfo *)(Array->user);

  Value *Size = getArraySize(Array);
  Value *Offset = getArrayOffset(Array);
  Value *DevPtr = DeviceAllocations[ScopArray];

  Value *HostPtr;

  if (gpu_array_is_scalar(Array))
    HostPtr = BlockGen.getOrCreateAlloca(ScopArray);
  else
    HostPtr = ScopArray->getBasePtr();

  if (Offset) {
    HostPtr = Builder.CreatePointerCast(
        HostPtr, ScopArray->getElementType()->getPointerTo());
    HostPtr = Builder.CreateGEP(HostPtr, Offset);
  }

  HostPtr = Builder.CreatePointerCast(HostPtr, Builder.getInt8PtrTy());

  if (Offset) {
    Size = Builder.CreateSub(
        Size, Builder.CreateMul(
                  Offset, Builder.getInt64(ScopArray->getElemSizeInBytes())));
  }

  if (Direction == HOST_TO_DEVICE)
    createCallCopyFromHostToDevice(HostPtr, DevPtr, Size);
  else
    createCallCopyFromDeviceToHost(DevPtr, HostPtr, Size);

  isl_id_free(Id);
  isl_ast_expr_free(Arg);
  isl_ast_expr_free(Expr);
  isl_ast_node_free(TransferStmt);
}

void GPUNodeBuilder::createUser(__isl_take isl_ast_node *UserStmt) {
  isl_ast_expr *Expr = isl_ast_node_user_get_expr(UserStmt);
  isl_ast_expr *StmtExpr = isl_ast_expr_get_op_arg(Expr, 0);
  isl_id *Id = isl_ast_expr_get_id(StmtExpr);
  isl_id_free(Id);
  isl_ast_expr_free(StmtExpr);

  const char *Str = isl_id_get_name(Id);
  if (!strcmp(Str, "kernel")) {
    createKernel(UserStmt);
    isl_ast_expr_free(Expr);
    return;
  }

  if (isPrefix(Str, "to_device")) {
    if (!ManagedMemory)
      createDataTransfer(UserStmt, HOST_TO_DEVICE);
    else
      isl_ast_node_free(UserStmt);

    isl_ast_expr_free(Expr);
    return;
  }

  if (isPrefix(Str, "from_device")) {
    if (!ManagedMemory) {
      createDataTransfer(UserStmt, DEVICE_TO_HOST);
    } else {
      createCallSynchronizeDevice();
      isl_ast_node_free(UserStmt);
    }
    isl_ast_expr_free(Expr);
    return;
  }

  isl_id *Anno = isl_ast_node_get_annotation(UserStmt);
  struct ppcg_kernel_stmt *KernelStmt =
      (struct ppcg_kernel_stmt *)isl_id_get_user(Anno);
  isl_id_free(Anno);

  switch (KernelStmt->type) {
  case ppcg_kernel_domain:
    createScopStmt(Expr, KernelStmt);
    isl_ast_node_free(UserStmt);
    return;
  case ppcg_kernel_copy:
    createKernelCopy(KernelStmt);
    isl_ast_expr_free(Expr);
    isl_ast_node_free(UserStmt);
    return;
  case ppcg_kernel_sync:
    createKernelSync();
    isl_ast_expr_free(Expr);
    isl_ast_node_free(UserStmt);
    return;
  }

  isl_ast_expr_free(Expr);
  isl_ast_node_free(UserStmt);
  return;
}
void GPUNodeBuilder::createKernelCopy(ppcg_kernel_stmt *KernelStmt) {
  isl_ast_expr *LocalIndex = isl_ast_expr_copy(KernelStmt->u.c.local_index);
  LocalIndex = isl_ast_expr_address_of(LocalIndex);
  Value *LocalAddr = ExprBuilder.create(LocalIndex);
  isl_ast_expr *Index = isl_ast_expr_copy(KernelStmt->u.c.index);
  Index = isl_ast_expr_address_of(Index);
  Value *GlobalAddr = ExprBuilder.create(Index);

  if (KernelStmt->u.c.read) {
    LoadInst *Load = Builder.CreateLoad(GlobalAddr, "shared.read");
    Builder.CreateStore(Load, LocalAddr);
  } else {
    LoadInst *Load = Builder.CreateLoad(LocalAddr, "shared.write");
    Builder.CreateStore(Load, GlobalAddr);
  }
}

void GPUNodeBuilder::createScopStmt(isl_ast_expr *Expr,
                                    ppcg_kernel_stmt *KernelStmt) {
  auto Stmt = (ScopStmt *)KernelStmt->u.d.stmt->stmt;
  isl_id_to_ast_expr *Indexes = KernelStmt->u.d.ref2expr;

  LoopToScevMapT LTS;
  LTS.insert(OutsideLoopIterations.begin(), OutsideLoopIterations.end());

  createSubstitutions(Expr, Stmt, LTS);

  if (Stmt->isBlockStmt())
    BlockGen.copyStmt(*Stmt, LTS, Indexes);
  else
    RegionGen.copyStmt(*Stmt, LTS, Indexes);
}

void GPUNodeBuilder::createKernelSync() {
  Module *M = Builder.GetInsertBlock()->getParent()->getParent();

  Function *Sync;

  switch (Arch) {
  case GPUArch::SPIR64:
  case GPUArch::SPIR32:
  case GPUArch::NVPTX64:
    Sync = Intrinsic::getDeclaration(M, Intrinsic::nvvm_barrier0);
    break;
  }

  Builder.CreateCall(Sync, {});
}

/// Collect llvm::Values referenced from @p Node
///
/// This function only applies to isl_ast_nodes that are user_nodes referring
/// to a ScopStmt. All other node types are ignore.
///
/// @param Node The node to collect references for.
/// @param User A user pointer used as storage for the data that is collected.
///
/// @returns isl_bool_true if data could be collected successfully.
isl_bool collectReferencesInGPUStmt(__isl_keep isl_ast_node *Node, void *User) {
  if (isl_ast_node_get_type(Node) != isl_ast_node_user)
    return isl_bool_true;

  isl_ast_expr *Expr = isl_ast_node_user_get_expr(Node);
  isl_ast_expr *StmtExpr = isl_ast_expr_get_op_arg(Expr, 0);
  isl_id *Id = isl_ast_expr_get_id(StmtExpr);
  const char *Str = isl_id_get_name(Id);
  isl_id_free(Id);
  isl_ast_expr_free(StmtExpr);
  isl_ast_expr_free(Expr);

  if (!isPrefix(Str, "Stmt"))
    return isl_bool_true;

  Id = isl_ast_node_get_annotation(Node);
  auto *KernelStmt = (ppcg_kernel_stmt *)isl_id_get_user(Id);
  auto Stmt = (ScopStmt *)KernelStmt->u.d.stmt->stmt;
  isl_id_free(Id);

  addReferencesFromStmt(Stmt, User, false /* CreateScalarRefs */);

  return isl_bool_true;
}

/// Check if F is a function that we can code-generate in a GPU kernel.
static bool isValidFunctionInKernel(llvm::Function *F) {
  assert(F && "F is an invalid pointer");
  // We string compare against the name of the function to allow
  // all variants of the intrinsic "llvm.sqrt.*"
  return F->isIntrinsic() && F->getName().startswith("llvm.sqrt");
}

/// Do not take `Function` as a subtree value.
///
/// We try to take the reference of all subtree values and pass them along
/// to the kernel from the host. Taking an address of any function and
/// trying to pass along is nonsensical. Only allow `Value`s that are not
/// `Function`s.
static bool isValidSubtreeValue(llvm::Value *V) { return !isa<Function>(V); }

/// Return `Function`s from `RawSubtreeValues`.
static SetVector<Function *>
getFunctionsFromRawSubtreeValues(SetVector<Value *> RawSubtreeValues) {
  SetVector<Function *> SubtreeFunctions;
  for (Value *It : RawSubtreeValues) {
    Function *F = dyn_cast<Function>(It);
    if (F) {
      assert(isValidFunctionInKernel(F) && "Code should have bailed out by "
                                           "this point if an invalid function "
                                           "were present in a kernel.");
      SubtreeFunctions.insert(F);
    }
  }
  return SubtreeFunctions;
}

std::pair<SetVector<Value *>, SetVector<Function *>>
GPUNodeBuilder::getReferencesInKernel(ppcg_kernel *Kernel) {
  SetVector<Value *> SubtreeValues;
  SetVector<const SCEV *> SCEVs;
  SetVector<const Loop *> Loops;
  SubtreeReferences References = {
      LI, SE, S, ValueMap, SubtreeValues, SCEVs, getBlockGenerator()};

  for (const auto &I : IDToValue)
    SubtreeValues.insert(I.second);

  isl_ast_node_foreach_descendant_top_down(
      Kernel->tree, collectReferencesInGPUStmt, &References);

  for (const SCEV *Expr : SCEVs)
    findValues(Expr, SE, SubtreeValues);

  for (auto &SAI : S.arrays())
    SubtreeValues.remove(SAI->getBasePtr());

  isl_space *Space = S.getParamSpace();
  for (long i = 0; i < isl_space_dim(Space, isl_dim_param); i++) {
    isl_id *Id = isl_space_get_dim_id(Space, isl_dim_param, i);
    assert(IDToValue.count(Id));
    Value *Val = IDToValue[Id];
    SubtreeValues.remove(Val);
    isl_id_free(Id);
  }
  isl_space_free(Space);

  for (long i = 0; i < isl_space_dim(Kernel->space, isl_dim_set); i++) {
    isl_id *Id = isl_space_get_dim_id(Kernel->space, isl_dim_set, i);
    assert(IDToValue.count(Id));
    Value *Val = IDToValue[Id];
    SubtreeValues.remove(Val);
    isl_id_free(Id);
  }

  // Note: { ValidSubtreeValues, ValidSubtreeFunctions } partitions
  // SubtreeValues. This is important, because we should not lose any
  // SubtreeValues in the process of constructing the
  // "ValidSubtree{Values, Functions} sets. Nor should the set
  // ValidSubtree{Values, Functions} have any common element.
  auto ValidSubtreeValuesIt =
      make_filter_range(SubtreeValues, isValidSubtreeValue);
  SetVector<Value *> ValidSubtreeValues(ValidSubtreeValuesIt.begin(),
                                        ValidSubtreeValuesIt.end());
  SetVector<Function *> ValidSubtreeFunctions(
      getFunctionsFromRawSubtreeValues(SubtreeValues));

  return std::make_pair(ValidSubtreeValues, ValidSubtreeFunctions);
}

void GPUNodeBuilder::clearDominators(Function *F) {
  DomTreeNode *N = DT.getNode(&F->getEntryBlock());
  std::vector<BasicBlock *> Nodes;
  for (po_iterator<DomTreeNode *> I = po_begin(N), E = po_end(N); I != E; ++I)
    Nodes.push_back(I->getBlock());

  for (BasicBlock *BB : Nodes)
    DT.eraseNode(BB);
}

void GPUNodeBuilder::clearScalarEvolution(Function *F) {
  for (BasicBlock &BB : *F) {
    Loop *L = LI.getLoopFor(&BB);
    if (L)
      SE.forgetLoop(L);
  }
}

void GPUNodeBuilder::clearLoops(Function *F) {
  for (BasicBlock &BB : *F) {
    Loop *L = LI.getLoopFor(&BB);
    if (L)
      SE.forgetLoop(L);
    LI.removeBlock(&BB);
  }
}

std::tuple<Value *, Value *> GPUNodeBuilder::getGridSizes(ppcg_kernel *Kernel) {
  std::vector<Value *> Sizes;
  isl_ast_build *Context = isl_ast_build_from_context(S.getContext());

  for (long i = 0; i < Kernel->n_grid; i++) {
    isl_pw_aff *Size = isl_multi_pw_aff_get_pw_aff(Kernel->grid_size, i);
    isl_ast_expr *GridSize = isl_ast_build_expr_from_pw_aff(Context, Size);
    Value *Res = ExprBuilder.create(GridSize);
    Res = Builder.CreateTrunc(Res, Builder.getInt32Ty());
    Sizes.push_back(Res);
  }
  isl_ast_build_free(Context);

  for (long i = Kernel->n_grid; i < 3; i++)
    Sizes.push_back(ConstantInt::get(Builder.getInt32Ty(), 1));

  return std::make_tuple(Sizes[0], Sizes[1]);
}

std::tuple<Value *, Value *, Value *>
GPUNodeBuilder::getBlockSizes(ppcg_kernel *Kernel) {
  std::vector<Value *> Sizes;

  for (long i = 0; i < Kernel->n_block; i++) {
    Value *Res = ConstantInt::get(Builder.getInt32Ty(), Kernel->block_dim[i]);
    Sizes.push_back(Res);
  }

  for (long i = Kernel->n_block; i < 3; i++)
    Sizes.push_back(ConstantInt::get(Builder.getInt32Ty(), 1));

  return std::make_tuple(Sizes[0], Sizes[1], Sizes[2]);
}

void GPUNodeBuilder::insertStoreParameter(Instruction *Parameters,
                                          Instruction *Param, int Index) {
  Value *Slot = Builder.CreateGEP(
      Parameters, {Builder.getInt64(0), Builder.getInt64(Index)});
  Value *ParamTyped = Builder.CreatePointerCast(Param, Builder.getInt8PtrTy());
  Builder.CreateStore(ParamTyped, Slot);
}

Value *
GPUNodeBuilder::createLaunchParameters(ppcg_kernel *Kernel, Function *F,
                                       SetVector<Value *> SubtreeValues) {
  const int NumArgs = F->arg_size();
  std::vector<int> ArgSizes(NumArgs);

  Type *ArrayTy = ArrayType::get(Builder.getInt8PtrTy(), 2 * NumArgs);

  BasicBlock *EntryBlock =
      &Builder.GetInsertBlock()->getParent()->getEntryBlock();
  auto AddressSpace = F->getParent()->getDataLayout().getAllocaAddrSpace();
  std::string Launch = "polly_launch_" + std::to_string(Kernel->id);
  Instruction *Parameters = new AllocaInst(
      ArrayTy, AddressSpace, Launch + "_params", EntryBlock->getTerminator());

  int Index = 0;
  for (long i = 0; i < Prog->n_array; i++) {
    if (!ppcg_kernel_requires_array_argument(Kernel, i))
      continue;

    isl_id *Id = isl_space_get_tuple_id(Prog->array[i].space, isl_dim_set);
    const ScopArrayInfo *SAI = ScopArrayInfo::getFromId(Id);

    ArgSizes[Index] = SAI->getElemSizeInBytes();

    Value *DevArray = nullptr;
    if (ManagedMemory) {
      DevArray = getOrCreateManagedDeviceArray(
          &Prog->array[i], const_cast<ScopArrayInfo *>(SAI));
    } else {
      DevArray = DeviceAllocations[const_cast<ScopArrayInfo *>(SAI)];
      DevArray = createCallGetDevicePtr(DevArray);
    }
    assert(DevArray != nullptr && "Array to be offloaded to device not "
                                  "initialized");
    Value *Offset = getArrayOffset(&Prog->array[i]);

    if (Offset) {
      DevArray = Builder.CreatePointerCast(
          DevArray, SAI->getElementType()->getPointerTo());
      DevArray = Builder.CreateGEP(DevArray, Builder.CreateNeg(Offset));
      DevArray = Builder.CreatePointerCast(DevArray, Builder.getInt8PtrTy());
    }
    Value *Slot = Builder.CreateGEP(
        Parameters, {Builder.getInt64(0), Builder.getInt64(Index)});

    if (gpu_array_is_read_only_scalar(&Prog->array[i])) {
      Value *ValPtr = nullptr;
      if (ManagedMemory)
        ValPtr = DevArray;
      else
        ValPtr = BlockGen.getOrCreateAlloca(SAI);

      assert(ValPtr != nullptr && "ValPtr that should point to a valid object"
                                  " to be stored into Parameters");
      Value *ValPtrCast =
          Builder.CreatePointerCast(ValPtr, Builder.getInt8PtrTy());
      Builder.CreateStore(ValPtrCast, Slot);
    } else {
      Instruction *Param =
          new AllocaInst(Builder.getInt8PtrTy(), AddressSpace,
                         Launch + "_param_" + std::to_string(Index),
                         EntryBlock->getTerminator());
      Builder.CreateStore(DevArray, Param);
      Value *ParamTyped =
          Builder.CreatePointerCast(Param, Builder.getInt8PtrTy());
      Builder.CreateStore(ParamTyped, Slot);
    }
    Index++;
  }

  int NumHostIters = isl_space_dim(Kernel->space, isl_dim_set);

  for (long i = 0; i < NumHostIters; i++) {
    isl_id *Id = isl_space_get_dim_id(Kernel->space, isl_dim_set, i);
    Value *Val = IDToValue[Id];
    isl_id_free(Id);

    ArgSizes[Index] = computeSizeInBytes(Val->getType());

    Instruction *Param =
        new AllocaInst(Val->getType(), AddressSpace,
                       Launch + "_param_" + std::to_string(Index),
                       EntryBlock->getTerminator());
    Builder.CreateStore(Val, Param);
    insertStoreParameter(Parameters, Param, Index);
    Index++;
  }

  int NumVars = isl_space_dim(Kernel->space, isl_dim_param);

  for (long i = 0; i < NumVars; i++) {
    isl_id *Id = isl_space_get_dim_id(Kernel->space, isl_dim_param, i);
    Value *Val = IDToValue[Id];
    isl_id_free(Id);

    ArgSizes[Index] = computeSizeInBytes(Val->getType());

    Instruction *Param =
        new AllocaInst(Val->getType(), AddressSpace,
                       Launch + "_param_" + std::to_string(Index),
                       EntryBlock->getTerminator());
    Builder.CreateStore(Val, Param);
    insertStoreParameter(Parameters, Param, Index);
    Index++;
  }

  for (auto Val : SubtreeValues) {
    ArgSizes[Index] = computeSizeInBytes(Val->getType());

    Instruction *Param =
        new AllocaInst(Val->getType(), AddressSpace,
                       Launch + "_param_" + std::to_string(Index),
                       EntryBlock->getTerminator());
    Builder.CreateStore(Val, Param);
    insertStoreParameter(Parameters, Param, Index);
    Index++;
  }

  for (int i = 0; i < NumArgs; i++) {
    Value *Val = ConstantInt::get(Builder.getInt32Ty(), ArgSizes[i]);
    Instruction *Param =
        new AllocaInst(Builder.getInt32Ty(), AddressSpace,
                       Launch + "_param_size_" + std::to_string(i),
                       EntryBlock->getTerminator());
    Builder.CreateStore(Val, Param);
    insertStoreParameter(Parameters, Param, Index);
    Index++;
  }

  auto Location = EntryBlock->getTerminator();
  return new BitCastInst(Parameters, Builder.getInt8PtrTy(),
                         Launch + "_params_i8ptr", Location);
}

void GPUNodeBuilder::setupKernelSubtreeFunctions(
    SetVector<Function *> SubtreeFunctions) {
  for (auto Fn : SubtreeFunctions) {
    const std::string ClonedFnName = Fn->getName();
    Function *Clone = GPUModule->getFunction(ClonedFnName);
    if (!Clone)
      Clone =
          Function::Create(Fn->getFunctionType(), GlobalValue::ExternalLinkage,
                           ClonedFnName, GPUModule.get());
    assert(Clone && "Expected cloned function to be initialized.");
    assert(ValueMap.find(Fn) == ValueMap.end() &&
           "Fn already present in ValueMap");
    ValueMap[Fn] = Clone;
  }
}
void GPUNodeBuilder::createKernel(__isl_take isl_ast_node *KernelStmt) {
  isl_id *Id = isl_ast_node_get_annotation(KernelStmt);
  ppcg_kernel *Kernel = (ppcg_kernel *)isl_id_get_user(Id);
  isl_id_free(Id);
  isl_ast_node_free(KernelStmt);

  if (Kernel->n_grid > 1)
    DeepestParallel =
        std::max(DeepestParallel, isl_space_dim(Kernel->space, isl_dim_set));
  else
    DeepestSequential =
        std::max(DeepestSequential, isl_space_dim(Kernel->space, isl_dim_set));

  Value *BlockDimX, *BlockDimY, *BlockDimZ;
  std::tie(BlockDimX, BlockDimY, BlockDimZ) = getBlockSizes(Kernel);

  SetVector<Value *> SubtreeValues;
  SetVector<Function *> SubtreeFunctions;
  std::tie(SubtreeValues, SubtreeFunctions) = getReferencesInKernel(Kernel);

  assert(Kernel->tree && "Device AST of kernel node is empty");

  Instruction &HostInsertPoint = *Builder.GetInsertPoint();
  IslExprBuilder::IDToValueTy HostIDs = IDToValue;
  ValueMapT HostValueMap = ValueMap;
  BlockGenerator::AllocaMapTy HostScalarMap = ScalarMap;
  ScalarMap.clear();

  SetVector<const Loop *> Loops;

  // Create for all loops we depend on values that contain the current loop
  // iteration. These values are necessary to generate code for SCEVs that
  // depend on such loops. As a result we need to pass them to the subfunction.
  for (const Loop *L : Loops) {
    const SCEV *OuterLIV = SE.getAddRecExpr(SE.getUnknown(Builder.getInt64(0)),
                                            SE.getUnknown(Builder.getInt64(1)),
                                            L, SCEV::FlagAnyWrap);
    Value *V = generateSCEV(OuterLIV);
    OutsideLoopIterations[L] = SE.getUnknown(V);
    SubtreeValues.insert(V);
  }

  createKernelFunction(Kernel, SubtreeValues, SubtreeFunctions);
  setupKernelSubtreeFunctions(SubtreeFunctions);

  create(isl_ast_node_copy(Kernel->tree));

  finalizeKernelArguments(Kernel);
  Function *F = Builder.GetInsertBlock()->getParent();
  if (Arch == GPUArch::NVPTX64)
    addCUDAAnnotations(F->getParent(), BlockDimX, BlockDimY, BlockDimZ);
  clearDominators(F);
  clearScalarEvolution(F);
  clearLoops(F);

  IDToValue = HostIDs;

  ValueMap = std::move(HostValueMap);
  ScalarMap = std::move(HostScalarMap);
  EscapeMap.clear();
  IDToSAI.clear();
  Annotator.resetAlternativeAliasBases();
  for (auto &BasePtr : LocalArrays)
    S.invalidateScopArrayInfo(BasePtr, MemoryKind::Array);
  LocalArrays.clear();

  std::string ASMString = finalizeKernelFunction();
  Builder.SetInsertPoint(&HostInsertPoint);
  Value *Parameters = createLaunchParameters(Kernel, F, SubtreeValues);

  std::string Name = getKernelFuncName(Kernel->id);
  Value *KernelString = Builder.CreateGlobalStringPtr(ASMString, Name);
  Value *NameString = Builder.CreateGlobalStringPtr(Name, Name + "_name");
  Value *GPUKernel = createCallGetKernel(KernelString, NameString);

  Value *GridDimX, *GridDimY;
  std::tie(GridDimX, GridDimY) = getGridSizes(Kernel);

  createCallLaunchKernel(GPUKernel, GridDimX, GridDimY, BlockDimX, BlockDimY,
                         BlockDimZ, Parameters);
  createCallFreeKernel(GPUKernel);

  for (auto Id : KernelIds)
    isl_id_free(Id);

  KernelIds.clear();
}

/// Compute the DataLayout string for the NVPTX backend.
///
/// @param is64Bit Are we looking for a 64 bit architecture?
static std::string computeNVPTXDataLayout(bool is64Bit) {
  std::string Ret = "";

  if (!is64Bit) {
    Ret += "e-p:32:32:32-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:"
           "64-f32:32:32-f64:64:64-v16:16:16-v32:32:32-v64:64:"
           "64-v128:128:128-n16:32:64";
  } else {
    Ret += "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:"
           "64-f32:32:32-f64:64:64-v16:16:16-v32:32:32-v64:64:"
           "64-v128:128:128-n16:32:64";
  }

  return Ret;
}

/// Compute the DataLayout string for a SPIR kernel.
///
/// @param is64Bit Are we looking for a 64 bit architecture?
static std::string computeSPIRDataLayout(bool is64Bit) {
  std::string Ret = "";

  if (!is64Bit) {
    Ret += "e-p:32:32:32-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:"
           "64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:"
           "32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:"
           "256:256-v256:256:256-v512:512:512-v1024:1024:1024";
  } else {
    Ret += "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:"
           "64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:"
           "32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:"
           "256:256-v256:256:256-v512:512:512-v1024:1024:1024";
  }

  return Ret;
}

Function *
GPUNodeBuilder::createKernelFunctionDecl(ppcg_kernel *Kernel,
                                         SetVector<Value *> &SubtreeValues) {
  std::vector<Type *> Args;
  std::string Identifier = getKernelFuncName(Kernel->id);

  std::vector<Metadata *> MemoryType;

  for (long i = 0; i < Prog->n_array; i++) {
    if (!ppcg_kernel_requires_array_argument(Kernel, i))
      continue;

    if (gpu_array_is_read_only_scalar(&Prog->array[i])) {
      isl_id *Id = isl_space_get_tuple_id(Prog->array[i].space, isl_dim_set);
      const ScopArrayInfo *SAI = ScopArrayInfo::getFromId(Id);
      Args.push_back(SAI->getElementType());
      MemoryType.push_back(ConstantAsMetadata::get(ConstantInt::get(
              Builder.getInt32Ty(), 0)));
    } else {
      static const int UseGlobalMemory = 1;
      Args.push_back(Builder.getInt8PtrTy(UseGlobalMemory));
      MemoryType.push_back(ConstantAsMetadata::get(ConstantInt::get(
              Builder.getInt32Ty(), 1)));
    }
  }

  int NumHostIters = isl_space_dim(Kernel->space, isl_dim_set);

  for (long i = 0; i < NumHostIters; i++) {
    Args.push_back(Builder.getInt64Ty());
    MemoryType.push_back(ConstantAsMetadata::get(ConstantInt::get(
            Builder.getInt32Ty(), 0)));
  }

  int NumVars = isl_space_dim(Kernel->space, isl_dim_param);

  for (long i = 0; i < NumVars; i++) {
    isl_id *Id = isl_space_get_dim_id(Kernel->space, isl_dim_param, i);
    Value *Val = IDToValue[Id];
    isl_id_free(Id);
    Args.push_back(Val->getType());
    MemoryType.push_back(ConstantAsMetadata::get(ConstantInt::get(
            Builder.getInt32Ty(), 0)));
  }

  for (auto *V : SubtreeValues) {
    Args.push_back(V->getType());
    MemoryType.push_back(ConstantAsMetadata::get(ConstantInt::get(
            Builder.getInt32Ty(), 0)));
  }

  auto *FT = FunctionType::get(Builder.getVoidTy(), Args, false);
  auto *FN = Function::Create(FT, Function::ExternalLinkage, Identifier,
                              GPUModule.get());

  std::vector<Metadata *> EmptyStrings;

  for (unsigned int i = 0; i < MemoryType.size(); i++) {
    EmptyStrings.push_back(MDString::get(FN->getContext(), ""));
  }

  if (Arch == GPUArch::SPIR32 || Arch == GPUArch::SPIR64) {
    FN->setMetadata("kernel_arg_addr_space", MDNode::get(FN->getContext(),
          MemoryType));
    FN->setMetadata("kernel_arg_name", MDNode::get(
          FN->getContext(), EmptyStrings));
    FN->setMetadata("kernel_arg_access_qual", MDNode::get(
          FN->getContext(), EmptyStrings));
    FN->setMetadata("kernel_arg_type", MDNode::get(
          FN->getContext(), EmptyStrings));
    FN->setMetadata("kernel_arg_type_qual", MDNode::get(
          FN->getContext(), EmptyStrings));
    FN->setMetadata("kernel_arg_base_type", MDNode::get(
          FN->getContext(), EmptyStrings));
  }

  switch (Arch) {
  case GPUArch::NVPTX64:
    FN->setCallingConv(CallingConv::PTX_Kernel);
    break;
  case GPUArch::SPIR32:
  case GPUArch::SPIR64:
    FN->setCallingConv(CallingConv::SPIR_KERNEL);
    break;
  }

  auto Arg = FN->arg_begin();
  for (long i = 0; i < Kernel->n_array; i++) {
    if (!ppcg_kernel_requires_array_argument(Kernel, i))
      continue;

    Arg->setName(Kernel->array[i].array->name);

    isl_id *Id = isl_space_get_tuple_id(Prog->array[i].space, isl_dim_set);
    const ScopArrayInfo *SAI = ScopArrayInfo::getFromId(isl_id_copy(Id));
    Type *EleTy = SAI->getElementType();
    Value *Val = &*Arg;
    SmallVector<const SCEV *, 4> Sizes;
    isl_ast_build *Build =
        isl_ast_build_from_context(isl_set_copy(Prog->context));
    Sizes.push_back(nullptr);
    for (long j = 1; j < Kernel->array[i].array->n_index; j++) {
      isl_ast_expr *DimSize = isl_ast_build_expr_from_pw_aff(
          Build, isl_pw_aff_copy(Kernel->array[i].array->bound[j]));
      auto V = ExprBuilder.create(DimSize);
      Sizes.push_back(SE.getSCEV(V));
    }
    const ScopArrayInfo *SAIRep =
        S.getOrCreateScopArrayInfo(Val, EleTy, Sizes, MemoryKind::Array);
    LocalArrays.push_back(Val);

    isl_ast_build_free(Build);
    KernelIds.push_back(Id);
    IDToSAI[Id] = SAIRep;
    Arg++;
  }

  for (long i = 0; i < NumHostIters; i++) {
    isl_id *Id = isl_space_get_dim_id(Kernel->space, isl_dim_set, i);
    Arg->setName(isl_id_get_name(Id));
    IDToValue[Id] = &*Arg;
    KernelIDs.insert(std::unique_ptr<isl_id, IslIdDeleter>(Id));
    Arg++;
  }

  for (long i = 0; i < NumVars; i++) {
    isl_id *Id = isl_space_get_dim_id(Kernel->space, isl_dim_param, i);
    Arg->setName(isl_id_get_name(Id));
    Value *Val = IDToValue[Id];
    ValueMap[Val] = &*Arg;
    IDToValue[Id] = &*Arg;
    KernelIDs.insert(std::unique_ptr<isl_id, IslIdDeleter>(Id));
    Arg++;
  }

  for (auto *V : SubtreeValues) {
    Arg->setName(V->getName());
    ValueMap[V] = &*Arg;
    Arg++;
  }

  return FN;
}

void GPUNodeBuilder::insertKernelIntrinsics(ppcg_kernel *Kernel) {
  Intrinsic::ID IntrinsicsBID[2];
  Intrinsic::ID IntrinsicsTID[3];

  switch (Arch) {
  case GPUArch::SPIR64:
  case GPUArch::SPIR32:
  case GPUArch::NVPTX64:
    IntrinsicsBID[0] = Intrinsic::nvvm_read_ptx_sreg_ctaid_x;
    IntrinsicsBID[1] = Intrinsic::nvvm_read_ptx_sreg_ctaid_y;

    IntrinsicsTID[0] = Intrinsic::nvvm_read_ptx_sreg_tid_x;
    IntrinsicsTID[1] = Intrinsic::nvvm_read_ptx_sreg_tid_y;
    IntrinsicsTID[2] = Intrinsic::nvvm_read_ptx_sreg_tid_z;
    break;
  }

  auto addId = [this](__isl_take isl_id *Id, Intrinsic::ID Intr) mutable {
    std::string Name = isl_id_get_name(Id);
    Module *M = Builder.GetInsertBlock()->getParent()->getParent();
    Function *IntrinsicFn = Intrinsic::getDeclaration(M, Intr);
    Value *Val = Builder.CreateCall(IntrinsicFn, {});
    Val = Builder.CreateIntCast(Val, Builder.getInt64Ty(), false, Name);
    IDToValue[Id] = Val;
    KernelIDs.insert(std::unique_ptr<isl_id, IslIdDeleter>(Id));
  };

  for (int i = 0; i < Kernel->n_grid; ++i) {
    isl_id *Id = isl_id_list_get_id(Kernel->block_ids, i);
    addId(Id, IntrinsicsBID[i]);
  }

  for (int i = 0; i < Kernel->n_block; ++i) {
    isl_id *Id = isl_id_list_get_id(Kernel->thread_ids, i);
    addId(Id, IntrinsicsTID[i]);
  }
}

void GPUNodeBuilder::prepareKernelArguments(ppcg_kernel *Kernel, Function *FN) {
  auto Arg = FN->arg_begin();
  for (long i = 0; i < Kernel->n_array; i++) {
    if (!ppcg_kernel_requires_array_argument(Kernel, i))
      continue;

    isl_id *Id = isl_space_get_tuple_id(Prog->array[i].space, isl_dim_set);
    const ScopArrayInfo *SAI = ScopArrayInfo::getFromId(isl_id_copy(Id));
    isl_id_free(Id);

    if (SAI->getNumberOfDimensions() > 0) {
      Arg++;
      continue;
    }

    Value *Val = &*Arg;

    if (!gpu_array_is_read_only_scalar(&Prog->array[i])) {
      Type *TypePtr = SAI->getElementType()->getPointerTo();
      Value *TypedArgPtr = Builder.CreatePointerCast(Val, TypePtr);
      Val = Builder.CreateLoad(TypedArgPtr);
    }

    Value *Alloca = BlockGen.getOrCreateAlloca(SAI);
    Builder.CreateStore(Val, Alloca);

    Arg++;
  }
}

void GPUNodeBuilder::finalizeKernelArguments(ppcg_kernel *Kernel) {
  auto *FN = Builder.GetInsertBlock()->getParent();
  auto Arg = FN->arg_begin();

  bool StoredScalar = false;
  for (long i = 0; i < Kernel->n_array; i++) {
    if (!ppcg_kernel_requires_array_argument(Kernel, i))
      continue;

    isl_id *Id = isl_space_get_tuple_id(Prog->array[i].space, isl_dim_set);
    const ScopArrayInfo *SAI = ScopArrayInfo::getFromId(isl_id_copy(Id));
    isl_id_free(Id);

    if (SAI->getNumberOfDimensions() > 0) {
      Arg++;
      continue;
    }

    if (gpu_array_is_read_only_scalar(&Prog->array[i])) {
      Arg++;
      continue;
    }

    Value *Alloca = BlockGen.getOrCreateAlloca(SAI);
    Value *ArgPtr = &*Arg;
    Type *TypePtr = SAI->getElementType()->getPointerTo();
    Value *TypedArgPtr = Builder.CreatePointerCast(ArgPtr, TypePtr);
    Value *Val = Builder.CreateLoad(Alloca);
    Builder.CreateStore(Val, TypedArgPtr);
    StoredScalar = true;

    Arg++;
  }

  if (StoredScalar)
    /// In case more than one thread contains scalar stores, the generated
    /// code might be incorrect, if we only store at the end of the kernel.
    /// To support this case we need to store these scalars back at each
    /// memory store or at least before each kernel barrier.
    if (Kernel->n_block != 0 || Kernel->n_grid != 0)
      BuildSuccessful = 0;
}

void GPUNodeBuilder::createKernelVariables(ppcg_kernel *Kernel, Function *FN) {
  Module *M = Builder.GetInsertBlock()->getParent()->getParent();

  for (int i = 0; i < Kernel->n_var; ++i) {
    struct ppcg_kernel_var &Var = Kernel->var[i];
    isl_id *Id = isl_space_get_tuple_id(Var.array->space, isl_dim_set);
    Type *EleTy = ScopArrayInfo::getFromId(Id)->getElementType();

    Type *ArrayTy = EleTy;
    SmallVector<const SCEV *, 4> Sizes;

    Sizes.push_back(nullptr);
    for (unsigned int j = 1; j < Var.array->n_index; ++j) {
      isl_val *Val = isl_vec_get_element_val(Var.size, j);
      long Bound = isl_val_get_num_si(Val);
      isl_val_free(Val);
      Sizes.push_back(S.getSE()->getConstant(Builder.getInt64Ty(), Bound));
    }

    for (int j = Var.array->n_index - 1; j >= 0; --j) {
      isl_val *Val = isl_vec_get_element_val(Var.size, j);
      long Bound = isl_val_get_num_si(Val);
      isl_val_free(Val);
      ArrayTy = ArrayType::get(ArrayTy, Bound);
    }

    const ScopArrayInfo *SAI;
    Value *Allocation;
    if (Var.type == ppcg_access_shared) {
      auto GlobalVar = new GlobalVariable(
          *M, ArrayTy, false, GlobalValue::InternalLinkage, 0, Var.name,
          nullptr, GlobalValue::ThreadLocalMode::NotThreadLocal, 3);
      GlobalVar->setAlignment(EleTy->getPrimitiveSizeInBits() / 8);
      GlobalVar->setInitializer(Constant::getNullValue(ArrayTy));

      Allocation = GlobalVar;
    } else if (Var.type == ppcg_access_private) {
      Allocation = Builder.CreateAlloca(ArrayTy, 0, "private_array");
    } else {
      llvm_unreachable("unknown variable type");
    }
    SAI =
        S.getOrCreateScopArrayInfo(Allocation, EleTy, Sizes, MemoryKind::Array);
    Id = isl_id_alloc(S.getIslCtx(), Var.name, nullptr);
    IDToValue[Id] = Allocation;
    LocalArrays.push_back(Allocation);
    KernelIds.push_back(Id);
    IDToSAI[Id] = SAI;
  }
}

void GPUNodeBuilder::createKernelFunction(
    ppcg_kernel *Kernel, SetVector<Value *> &SubtreeValues,
    SetVector<Function *> &SubtreeFunctions) {
  std::string Identifier = getKernelFuncName(Kernel->id);
  GPUModule.reset(new Module(Identifier, Builder.getContext()));

  switch (Arch) {
  case GPUArch::NVPTX64:
    if (Runtime == GPURuntime::CUDA)
      GPUModule->setTargetTriple(Triple::normalize("nvptx64-nvidia-cuda"));
    else if (Runtime == GPURuntime::OpenCL)
      GPUModule->setTargetTriple(Triple::normalize("nvptx64-nvidia-nvcl"));
    GPUModule->setDataLayout(computeNVPTXDataLayout(true /* is64Bit */));
    break;
  case GPUArch::SPIR32:
    GPUModule->setTargetTriple(Triple::normalize("spir-unknown-unknown"));
    GPUModule->setDataLayout(computeSPIRDataLayout(false /* is64Bit */));
    break;
  case GPUArch::SPIR64:
    GPUModule->setTargetTriple(Triple::normalize("spir64-unknown-unknown"));
    GPUModule->setDataLayout(computeSPIRDataLayout(true /* is64Bit */));
    break;
  }

  Function *FN = createKernelFunctionDecl(Kernel, SubtreeValues);

  BasicBlock *PrevBlock = Builder.GetInsertBlock();
  auto EntryBlock = BasicBlock::Create(Builder.getContext(), "entry", FN);

  DT.addNewBlock(EntryBlock, PrevBlock);

  Builder.SetInsertPoint(EntryBlock);
  Builder.CreateRetVoid();
  Builder.SetInsertPoint(EntryBlock, EntryBlock->begin());

  ScopDetection::markFunctionAsInvalid(FN);

  prepareKernelArguments(Kernel, FN);
  createKernelVariables(Kernel, FN);
  insertKernelIntrinsics(Kernel);
}

std::string GPUNodeBuilder::createKernelASM() {
  llvm::Triple GPUTriple;

  switch (Arch) {
  case GPUArch::NVPTX64:
    switch (Runtime) {
    case GPURuntime::CUDA:
      GPUTriple = llvm::Triple(Triple::normalize("nvptx64-nvidia-cuda"));
      break;
    case GPURuntime::OpenCL:
      GPUTriple = llvm::Triple(Triple::normalize("nvptx64-nvidia-nvcl"));
      break;
    }
    break;
  case GPUArch::SPIR64:
  case GPUArch::SPIR32:
    llvm_unreachable("Cannot generate ASM for SPIR architecture");
    break;
  }

  std::string ErrMsg;
  auto GPUTarget = TargetRegistry::lookupTarget(GPUTriple.getTriple(), ErrMsg);

  if (!GPUTarget) {
    errs() << ErrMsg << "\n";
    return "";
  }

  TargetOptions Options;
  Options.UnsafeFPMath = FastMath;

  std::string subtarget;

  switch (Arch) {
  case GPUArch::NVPTX64:
    subtarget = CudaVersion;
    break;
  case GPUArch::SPIR32:
  case GPUArch::SPIR64:
    llvm_unreachable("No subtarget for SPIR architecture");
    break;
  }

  std::unique_ptr<TargetMachine> TargetM(GPUTarget->createTargetMachine(
      GPUTriple.getTriple(), subtarget, "", Options, Optional<Reloc::Model>()));

  SmallString<0> ASMString;
  raw_svector_ostream ASMStream(ASMString);
  llvm::legacy::PassManager PM;

  PM.add(createTargetTransformInfoWrapperPass(TargetM->getTargetIRAnalysis()));

  if (TargetM->addPassesToEmitFile(
          PM, ASMStream, TargetMachine::CGFT_AssemblyFile, true /* verify */)) {
    errs() << "The target does not support generation of this file type!\n";
    return "";
  }

  PM.run(*GPUModule);

  return ASMStream.str();
}

std::string StringReplace(std::string const &in, std::string const &replace,
                          std::string const &with) {
  return std::regex_replace(in, std::regex(replace), with);
}

std::string GPUNodeBuilder::createKernelSPIR(std::string IR) {
  IR = StringReplace(IR, "declare i32 @llvm.nvvm.read.ptx.sreg.tid.x\\(\\)",
                     "declare spir_func i32 @__gen_ocl_get_local_id0()");
  IR = StringReplace(IR, "declare i32 @llvm.nvvm.read.ptx.sreg.tid.y\\(\\)",
                     "declare spir_func i32 @__gen_ocl_get_local_id1()");
  IR = StringReplace(IR, "declare i32 @llvm.nvvm.read.ptx.sreg.tid.z\\(\\)",
                     "declare spir_func i32 @__gen_ocl_get_local_id2()");

  IR = StringReplace(IR, "declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x\\(\\)",
                     "declare spir_func i32 @__gen_ocl_get_group_id0()");
  IR = StringReplace(IR, "declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.y\\(\\)",
                     "declare spir_func i32 @__gen_ocl_get_group_id1()");
  IR = StringReplace(IR, "declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.z\\(\\)",
                     "declare spir_func i32 @__gen_ocl_get_group_id2()");

  IR = StringReplace(IR, "call i32 @llvm.nvvm.read.ptx.sreg.tid.x\\(\\)",
                     "call spir_func i32 @__gen_ocl_get_local_id0()");
  IR = StringReplace(IR, "call i32 @llvm.nvvm.read.ptx.sreg.tid.y\\(\\)",
                     "call spir_func i32 @__gen_ocl_get_local_id1()");
  IR = StringReplace(IR, "call i32 @llvm.nvvm.read.ptx.sreg.tid.z\\(\\)",
                     "call spir_func i32 @__gen_ocl_get_local_id2()");

  IR = StringReplace(IR, "call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x\\(\\)",
                     "call spir_func i32 @__gen_ocl_get_group_id0()");
  IR = StringReplace(IR, "call i32 @llvm.nvvm.read.ptx.sreg.ctaid.y\\(\\)",
                     "call spir_func i32 @__gen_ocl_get_group_id1()");
  IR = StringReplace(IR, "call i32 @llvm.nvvm.read.ptx.sreg.ctaid.z\\(\\)",
                     "call spir_func i32 @__gen_ocl_get_group_id2()");

  IR = StringReplace(IR, "declare void @llvm.nvvm.barrier0\\(\\)",
                     "declare spir_func void @__gen_ocl_barrier_global()");
  IR = StringReplace(IR, "call void @llvm.nvvm.barrier0\\(\\)",
                     "call spir_func void @__gen_ocl_barrier_global()");

   return IR;
}

std::string GPUNodeBuilder::finalizeKernelFunction() {

  if (verifyModule(*GPUModule)) {
    DEBUG(dbgs() << "verifyModule failed on module:\n";
          GPUModule->print(dbgs(), nullptr); dbgs() << "\n";);

    if (FailOnVerifyModuleFailure)
      llvm_unreachable("VerifyModule failed.");

    BuildSuccessful = false;
    return "";
  }

  if (DumpKernelIR)
    outs() << *GPUModule << "\n";

  if (Arch != GPUArch::SPIR32 && Arch != GPUArch::SPIR64) {
    // Optimize module.
    llvm::legacy::PassManager OptPasses;
    PassManagerBuilder PassBuilder;
    PassBuilder.OptLevel = 3;
    PassBuilder.SizeLevel = 0;
    PassBuilder.populateModulePassManager(OptPasses);
    OptPasses.run(*GPUModule);
  }

  std::string Assembly;

  if (Arch == GPUArch::SPIR32 || Arch == GPUArch::SPIR64) {
    std::string IR;
    raw_string_ostream IROstream(IR);
    IROstream << *GPUModule;
    IROstream.flush();
    Assembly = createKernelSPIR(IR);
  } else {
    Assembly = createKernelASM();
  }

  if (DumpKernelASM)
    outs() << Assembly << "\n";

  GPUModule.release();
  KernelIDs.clear();

  return Assembly;
}

namespace {
class PPCGCodeGeneration : public ScopPass {
public:
  static char ID;

  GPURuntime Runtime = GPURuntime::CUDA;

  GPUArch Architecture = GPUArch::NVPTX64;

  /// The scop that is currently processed.
  Scop *S;

  LoopInfo *LI;
  DominatorTree *DT;
  ScalarEvolution *SE;
  const DataLayout *DL;
  RegionInfo *RI;

  PPCGCodeGeneration() : ScopPass(ID) {}

  /// Construct compilation options for PPCG.
  ///
  /// @returns The compilation options.
  ppcg_options *createPPCGOptions() {
    auto DebugOptions =
        (ppcg_debug_options *)malloc(sizeof(ppcg_debug_options));
    auto Options = (ppcg_options *)malloc(sizeof(ppcg_options));

    DebugOptions->dump_schedule_constraints = false;
    DebugOptions->dump_schedule = false;
    DebugOptions->dump_final_schedule = false;
    DebugOptions->dump_sizes = false;
    DebugOptions->verbose = false;

    Options->debug = DebugOptions;

    Options->reschedule = true;
    Options->scale_tile_loops = false;
    Options->wrap = false;

    Options->non_negative_parameters = false;
    Options->ctx = nullptr;
    Options->sizes = nullptr;

    Options->tile_size = 32;

    Options->use_private_memory = PrivateMemory;
    Options->use_shared_memory = SharedMemory;
    Options->max_shared_memory = 48 * 1024;

    Options->target = PPCG_TARGET_CUDA;
    Options->openmp = false;
    Options->linearize_device_arrays = true;
    Options->live_range_reordering = false;

    Options->opencl_compiler_options = nullptr;
    Options->opencl_use_gpu = false;
    Options->opencl_n_include_file = 0;
    Options->opencl_include_files = nullptr;
    Options->opencl_print_kernel_types = false;
    Options->opencl_embed_kernel_code = false;

    Options->save_schedule_file = nullptr;
    Options->load_schedule_file = nullptr;

    return Options;
  }

  /// Get a tagged access relation containing all accesses of type @p AccessTy.
  ///
  /// Instead of a normal access of the form:
  ///
  ///   Stmt[i,j,k] -> Array[f_0(i,j,k), f_1(i,j,k)]
  ///
  /// a tagged access has the form
  ///
  ///   [Stmt[i,j,k] -> id[]] -> Array[f_0(i,j,k), f_1(i,j,k)]
  ///
  /// where 'id' is an additional space that references the memory access that
  /// triggered the access.
  ///
  /// @param AccessTy The type of the memory accesses to collect.
  ///
  /// @return The relation describing all tagged memory accesses.
  isl_union_map *getTaggedAccesses(enum MemoryAccess::AccessType AccessTy) {
    isl_union_map *Accesses = isl_union_map_empty(S->getParamSpace());

    for (auto &Stmt : *S)
      for (auto &Acc : Stmt)
        if (Acc->getType() == AccessTy) {
          isl_map *Relation = Acc->getAccessRelation();
          Relation = isl_map_intersect_domain(Relation, Stmt.getDomain());

          isl_space *Space = isl_map_get_space(Relation);
          Space = isl_space_range(Space);
          Space = isl_space_from_range(Space);
          Space = isl_space_set_tuple_id(Space, isl_dim_in, Acc->getId());
          isl_map *Universe = isl_map_universe(Space);
          Relation = isl_map_domain_product(Relation, Universe);
          Accesses = isl_union_map_add_map(Accesses, Relation);
        }

    return Accesses;
  }

  /// Get the set of all read accesses, tagged with the access id.
  ///
  /// @see getTaggedAccesses
  isl_union_map *getTaggedReads() {
    return getTaggedAccesses(MemoryAccess::READ);
  }

  /// Get the set of all may (and must) accesses, tagged with the access id.
  ///
  /// @see getTaggedAccesses
  isl_union_map *getTaggedMayWrites() {
    return isl_union_map_union(getTaggedAccesses(MemoryAccess::MAY_WRITE),
                               getTaggedAccesses(MemoryAccess::MUST_WRITE));
  }

  /// Get the set of all must accesses, tagged with the access id.
  ///
  /// @see getTaggedAccesses
  isl_union_map *getTaggedMustWrites() {
    return getTaggedAccesses(MemoryAccess::MUST_WRITE);
  }

  /// Collect parameter and array names as isl_ids.
  ///
  /// To reason about the different parameters and arrays used, ppcg requires
  /// a list of all isl_ids in use. As PPCG traditionally performs
  /// source-to-source compilation each of these isl_ids is mapped to the
  /// expression that represents it. As we do not have a corresponding
  /// expression in Polly, we just map each id to a 'zero' expression to match
  /// the data format that ppcg expects.
  ///
  /// @returns Retun a map from collected ids to 'zero' ast expressions.
  __isl_give isl_id_to_ast_expr *getNames() {
    auto *Names = isl_id_to_ast_expr_alloc(
        S->getIslCtx(),
        S->getNumParams() + std::distance(S->array_begin(), S->array_end()));
    auto *Zero = isl_ast_expr_from_val(isl_val_zero(S->getIslCtx()));
    auto *Space = S->getParamSpace();

    for (int I = 0, E = S->getNumParams(); I < E; ++I) {
      isl_id *Id = isl_space_get_dim_id(Space, isl_dim_param, I);
      Names = isl_id_to_ast_expr_set(Names, Id, isl_ast_expr_copy(Zero));
    }

    for (auto &Array : S->arrays()) {
      auto Id = Array->getBasePtrId();
      Names = isl_id_to_ast_expr_set(Names, Id, isl_ast_expr_copy(Zero));
    }

    isl_space_free(Space);
    isl_ast_expr_free(Zero);

    return Names;
  }

  /// Create a new PPCG scop from the current scop.
  ///
  /// The PPCG scop is initialized with data from the current polly::Scop. From
  /// this initial data, the data-dependences in the PPCG scop are initialized.
  /// We do not use Polly's dependence analysis for now, to ensure we match
  /// the PPCG default behaviour more closely.
  ///
  /// @returns A new ppcg scop.
  ppcg_scop *createPPCGScop() {
    auto PPCGScop = (ppcg_scop *)malloc(sizeof(ppcg_scop));

    PPCGScop->options = createPPCGOptions();
    // enable live range reordering
    PPCGScop->options->live_range_reordering = 1;

    PPCGScop->start = 0;
    PPCGScop->end = 0;

    PPCGScop->context = S->getContext();
    PPCGScop->domain = S->getDomains();
    PPCGScop->call = nullptr;
    PPCGScop->tagged_reads = getTaggedReads();
    PPCGScop->reads = S->getReads();
    PPCGScop->live_in = nullptr;
    PPCGScop->tagged_may_writes = getTaggedMayWrites();
    PPCGScop->may_writes = S->getWrites();
    PPCGScop->tagged_must_writes = getTaggedMustWrites();
    PPCGScop->must_writes = S->getMustWrites();
    PPCGScop->live_out = nullptr;
    PPCGScop->tagger = nullptr;
    PPCGScop->independence =
        isl_union_map_empty(isl_set_get_space(PPCGScop->context));
    PPCGScop->dep_flow = nullptr;
    PPCGScop->tagged_dep_flow = nullptr;
    PPCGScop->dep_false = nullptr;
    PPCGScop->dep_forced = nullptr;
    PPCGScop->dep_order = nullptr;
    PPCGScop->tagged_dep_order = nullptr;

    PPCGScop->schedule = S->getScheduleTree();

    MustKillsInfo KillsInfo = computeMustKillsInfo(*S);
    // If we have something non-trivial to kill, add it to the schedule
    if (KillsInfo.KillsSchedule.get())
      PPCGScop->schedule = isl_schedule_sequence(
          PPCGScop->schedule, KillsInfo.KillsSchedule.take());
    PPCGScop->tagged_must_kills = KillsInfo.TaggedMustKills.take();

    PPCGScop->names = getNames();
    PPCGScop->pet = nullptr;

    compute_tagger(PPCGScop);
    compute_dependences(PPCGScop);

    return PPCGScop;
  }

  /// Collect the array accesses in a statement.
  ///
  /// @param Stmt The statement for which to collect the accesses.
  ///
  /// @returns A list of array accesses.
  gpu_stmt_access *getStmtAccesses(ScopStmt &Stmt) {
    gpu_stmt_access *Accesses = nullptr;

    for (MemoryAccess *Acc : Stmt) {
      auto Access = isl_alloc_type(S->getIslCtx(), struct gpu_stmt_access);
      Access->read = Acc->isRead();
      Access->write = Acc->isWrite();
      Access->access = Acc->getAccessRelation();
      isl_space *Space = isl_map_get_space(Access->access);
      Space = isl_space_range(Space);
      Space = isl_space_from_range(Space);
      Space = isl_space_set_tuple_id(Space, isl_dim_in, Acc->getId());
      isl_map *Universe = isl_map_universe(Space);
      Access->tagged_access =
          isl_map_domain_product(Acc->getAccessRelation(), Universe);
      Access->exact_write = !Acc->isMayWrite();
      Access->ref_id = Acc->getId();
      Access->next = Accesses;
      Access->n_index = Acc->getScopArrayInfo()->getNumberOfDimensions();
      Accesses = Access;
    }

    return Accesses;
  }

  /// Collect the list of GPU statements.
  ///
  /// Each statement has an id, a pointer to the underlying data structure,
  /// as well as a list with all memory accesses.
  ///
  /// TODO: Initialize the list of memory accesses.
  ///
  /// @returns A linked-list of statements.
  gpu_stmt *getStatements() {
    gpu_stmt *Stmts = isl_calloc_array(S->getIslCtx(), struct gpu_stmt,
                                       std::distance(S->begin(), S->end()));

    int i = 0;
    for (auto &Stmt : *S) {
      gpu_stmt *GPUStmt = &Stmts[i];

      GPUStmt->id = Stmt.getDomainId();

      // We use the pet stmt pointer to keep track of the Polly statements.
      GPUStmt->stmt = (pet_stmt *)&Stmt;
      GPUStmt->accesses = getStmtAccesses(Stmt);
      i++;
    }

    return Stmts;
  }

  /// Derive the extent of an array.
  ///
  /// The extent of an array is the set of elements that are within the
  /// accessed array. For the inner dimensions, the extent constraints are
  /// 0 and the size of the corresponding array dimension. For the first
  /// (outermost) dimension, the extent constraints are the minimal and maximal
  /// subscript value for the first dimension.
  ///
  /// @param Array The array to derive the extent for.
  ///
  /// @returns An isl_set describing the extent of the array.
  __isl_give isl_set *getExtent(ScopArrayInfo *Array) {
    unsigned NumDims = Array->getNumberOfDimensions();
    isl_union_map *Accesses = S->getAccesses();
    Accesses = isl_union_map_intersect_domain(Accesses, S->getDomains());
    Accesses = isl_union_map_detect_equalities(Accesses);
    isl_union_set *AccessUSet = isl_union_map_range(Accesses);
    AccessUSet = isl_union_set_coalesce(AccessUSet);
    AccessUSet = isl_union_set_detect_equalities(AccessUSet);
    AccessUSet = isl_union_set_coalesce(AccessUSet);

    if (isl_union_set_is_empty(AccessUSet)) {
      isl_union_set_free(AccessUSet);
      return isl_set_empty(Array->getSpace());
    }

    if (Array->getNumberOfDimensions() == 0) {
      isl_union_set_free(AccessUSet);
      return isl_set_universe(Array->getSpace());
    }

    isl_set *AccessSet =
        isl_union_set_extract_set(AccessUSet, Array->getSpace());

    isl_union_set_free(AccessUSet);
    isl_local_space *LS = isl_local_space_from_space(Array->getSpace());

    isl_pw_aff *Val =
        isl_pw_aff_from_aff(isl_aff_var_on_domain(LS, isl_dim_set, 0));

    isl_pw_aff *OuterMin = isl_set_dim_min(isl_set_copy(AccessSet), 0);
    isl_pw_aff *OuterMax = isl_set_dim_max(AccessSet, 0);
    OuterMin = isl_pw_aff_add_dims(OuterMin, isl_dim_in,
                                   isl_pw_aff_dim(Val, isl_dim_in));
    OuterMax = isl_pw_aff_add_dims(OuterMax, isl_dim_in,
                                   isl_pw_aff_dim(Val, isl_dim_in));
    OuterMin =
        isl_pw_aff_set_tuple_id(OuterMin, isl_dim_in, Array->getBasePtrId());
    OuterMax =
        isl_pw_aff_set_tuple_id(OuterMax, isl_dim_in, Array->getBasePtrId());

    isl_set *Extent = isl_set_universe(Array->getSpace());

    Extent = isl_set_intersect(
        Extent, isl_pw_aff_le_set(OuterMin, isl_pw_aff_copy(Val)));
    Extent = isl_set_intersect(Extent, isl_pw_aff_ge_set(OuterMax, Val));

    for (unsigned i = 1; i < NumDims; ++i)
      Extent = isl_set_lower_bound_si(Extent, isl_dim_set, i, 0);

    for (unsigned i = 0; i < NumDims; ++i) {
      isl_pw_aff *PwAff =
          const_cast<isl_pw_aff *>(Array->getDimensionSizePw(i));

      // isl_pw_aff can be NULL for zero dimension. Only in the case of a
      // Fortran array will we have a legitimate dimension.
      if (!PwAff) {
        assert(i == 0 && "invalid dimension isl_pw_aff for nonzero dimension");
        continue;
      }

      isl_pw_aff *Val = isl_pw_aff_from_aff(isl_aff_var_on_domain(
          isl_local_space_from_space(Array->getSpace()), isl_dim_set, i));
      PwAff = isl_pw_aff_add_dims(PwAff, isl_dim_in,
                                  isl_pw_aff_dim(Val, isl_dim_in));
      PwAff = isl_pw_aff_set_tuple_id(PwAff, isl_dim_in,
                                      isl_pw_aff_get_tuple_id(Val, isl_dim_in));
      auto *Set = isl_pw_aff_gt_set(PwAff, Val);
      Extent = isl_set_intersect(Set, Extent);
    }

    return Extent;
  }

  /// Derive the bounds of an array.
  ///
  /// For the first dimension we derive the bound of the array from the extent
  /// of this dimension. For inner dimensions we obtain their size directly from
  /// ScopArrayInfo.
  ///
  /// @param PPCGArray The array to compute bounds for.
  /// @param Array The polly array from which to take the information.
  void setArrayBounds(gpu_array_info &PPCGArray, ScopArrayInfo *Array) {
    if (PPCGArray.n_index > 0) {
      if (isl_set_is_empty(PPCGArray.extent)) {
        isl_set *Dom = isl_set_copy(PPCGArray.extent);
        isl_local_space *LS = isl_local_space_from_space(
            isl_space_params(isl_set_get_space(Dom)));
        isl_set_free(Dom);
        isl_aff *Zero = isl_aff_zero_on_domain(LS);
        PPCGArray.bound[0] = isl_pw_aff_from_aff(Zero);
      } else {
        isl_set *Dom = isl_set_copy(PPCGArray.extent);
        Dom = isl_set_project_out(Dom, isl_dim_set, 1, PPCGArray.n_index - 1);
        isl_pw_aff *Bound = isl_set_dim_max(isl_set_copy(Dom), 0);
        isl_set_free(Dom);
        Dom = isl_pw_aff_domain(isl_pw_aff_copy(Bound));
        isl_local_space *LS =
            isl_local_space_from_space(isl_set_get_space(Dom));
        isl_aff *One = isl_aff_zero_on_domain(LS);
        One = isl_aff_add_constant_si(One, 1);
        Bound = isl_pw_aff_add(Bound, isl_pw_aff_alloc(Dom, One));
        Bound = isl_pw_aff_gist(Bound, S->getContext());
        PPCGArray.bound[0] = Bound;
      }
    }

    for (unsigned i = 1; i < PPCGArray.n_index; ++i) {
      isl_pw_aff *Bound = Array->getDimensionSizePw(i);
      auto LS = isl_pw_aff_get_domain_space(Bound);
      auto Aff = isl_multi_aff_zero(LS);
      Bound = isl_pw_aff_pullback_multi_aff(Bound, Aff);
      PPCGArray.bound[i] = Bound;
    }
  }

  /// Create the arrays for @p PPCGProg.
  ///
  /// @param PPCGProg The program to compute the arrays for.
  void createArrays(gpu_prog *PPCGProg) {
    int i = 0;
    for (auto &Array : S->arrays()) {
      std::string TypeName;
      raw_string_ostream OS(TypeName);

      OS << *Array->getElementType();
      TypeName = OS.str();

      gpu_array_info &PPCGArray = PPCGProg->array[i];

      PPCGArray.space = Array->getSpace();
      PPCGArray.type = strdup(TypeName.c_str());
      PPCGArray.size = Array->getElementType()->getPrimitiveSizeInBits() / 8;
      PPCGArray.name = strdup(Array->getName().c_str());
      PPCGArray.extent = nullptr;
      PPCGArray.n_index = Array->getNumberOfDimensions();
      PPCGArray.bound =
          isl_alloc_array(S->getIslCtx(), isl_pw_aff *, PPCGArray.n_index);
      PPCGArray.extent = getExtent(Array);
      PPCGArray.n_ref = 0;
      PPCGArray.refs = nullptr;
      PPCGArray.accessed = true;
      PPCGArray.read_only_scalar =
          Array->isReadOnly() && Array->getNumberOfDimensions() == 0;
      PPCGArray.has_compound_element = false;
      PPCGArray.local = false;
      PPCGArray.declare_local = false;
      PPCGArray.global = false;
      PPCGArray.linearize = false;
      PPCGArray.dep_order = nullptr;
      PPCGArray.user = Array;

      setArrayBounds(PPCGArray, Array);
      i++;

      collect_references(PPCGProg, &PPCGArray);
    }
  }

  /// Create an identity map between the arrays in the scop.
  ///
  /// @returns An identity map between the arrays in the scop.
  isl_union_map *getArrayIdentity() {
    isl_union_map *Maps = isl_union_map_empty(S->getParamSpace());

    for (auto &Array : S->arrays()) {
      isl_space *Space = Array->getSpace();
      Space = isl_space_map_from_set(Space);
      isl_map *Identity = isl_map_identity(Space);
      Maps = isl_union_map_add_map(Maps, Identity);
    }

    return Maps;
  }

  /// Create a default-initialized PPCG GPU program.
  ///
  /// @returns A new gpu program description.
  gpu_prog *createPPCGProg(ppcg_scop *PPCGScop) {

    if (!PPCGScop)
      return nullptr;

    auto PPCGProg = isl_calloc_type(S->getIslCtx(), struct gpu_prog);

    PPCGProg->ctx = S->getIslCtx();
    PPCGProg->scop = PPCGScop;
    PPCGProg->context = isl_set_copy(PPCGScop->context);
    PPCGProg->read = isl_union_map_copy(PPCGScop->reads);
    PPCGProg->may_write = isl_union_map_copy(PPCGScop->may_writes);
    PPCGProg->must_write = isl_union_map_copy(PPCGScop->must_writes);
    PPCGProg->tagged_must_kill =
        isl_union_map_copy(PPCGScop->tagged_must_kills);
    PPCGProg->to_inner = getArrayIdentity();
    PPCGProg->to_outer = getArrayIdentity();
    PPCGProg->any_to_outer = nullptr;

    // this needs to be set when live range reordering is enabled.
    // NOTE: I believe that is conservatively correct. I'm not sure
    //       what the semantics of this is.
    // Quoting PPCG/gpu.h: "Order dependences on non-scalars."
    PPCGProg->array_order =
        isl_union_map_empty(isl_set_get_space(PPCGScop->context));
    PPCGProg->n_stmts = std::distance(S->begin(), S->end());
    PPCGProg->stmts = getStatements();
    PPCGProg->n_array = std::distance(S->array_begin(), S->array_end());
    PPCGProg->array = isl_calloc_array(S->getIslCtx(), struct gpu_array_info,
                                       PPCGProg->n_array);

    createArrays(PPCGProg);

    PPCGProg->may_persist = compute_may_persist(PPCGProg);
    return PPCGProg;
  }

  struct PrintGPUUserData {
    struct cuda_info *CudaInfo;
    struct gpu_prog *PPCGProg;
    std::vector<ppcg_kernel *> Kernels;
  };

  /// Print a user statement node in the host code.
  ///
  /// We use ppcg's printing facilities to print the actual statement and
  /// additionally build up a list of all kernels that are encountered in the
  /// host ast.
  ///
  /// @param P The printer to print to
  /// @param Options The printing options to use
  /// @param Node The node to print
  /// @param User A user pointer to carry additional data. This pointer is
  ///             expected to be of type PrintGPUUserData.
  ///
  /// @returns A printer to which the output has been printed.
  static __isl_give isl_printer *
  printHostUser(__isl_take isl_printer *P,
                __isl_take isl_ast_print_options *Options,
                __isl_take isl_ast_node *Node, void *User) {
    auto Data = (struct PrintGPUUserData *)User;
    auto Id = isl_ast_node_get_annotation(Node);

    if (Id) {
      bool IsUser = !strcmp(isl_id_get_name(Id), "user");

      // If this is a user statement, format it ourselves as ppcg would
      // otherwise try to call pet functionality that is not available in
      // Polly.
      if (IsUser) {
        P = isl_printer_start_line(P);
        P = isl_printer_print_ast_node(P, Node);
        P = isl_printer_end_line(P);
        isl_id_free(Id);
        isl_ast_print_options_free(Options);
        return P;
      }

      auto Kernel = (struct ppcg_kernel *)isl_id_get_user(Id);
      isl_id_free(Id);
      Data->Kernels.push_back(Kernel);
    }

    return print_host_user(P, Options, Node, User);
  }

  /// Print C code corresponding to the control flow in @p Kernel.
  ///
  /// @param Kernel The kernel to print
  void printKernel(ppcg_kernel *Kernel) {
    auto *P = isl_printer_to_str(S->getIslCtx());
    P = isl_printer_set_output_format(P, ISL_FORMAT_C);
    auto *Options = isl_ast_print_options_alloc(S->getIslCtx());
    P = isl_ast_node_print(Kernel->tree, P, Options);
    char *String = isl_printer_get_str(P);
    printf("%s\n", String);
    free(String);
    isl_printer_free(P);
  }

  /// Print C code corresponding to the GPU code described by @p Tree.
  ///
  /// @param Tree An AST describing GPU code
  /// @param PPCGProg The PPCG program from which @Tree has been constructed.
  void printGPUTree(isl_ast_node *Tree, gpu_prog *PPCGProg) {
    auto *P = isl_printer_to_str(S->getIslCtx());
    P = isl_printer_set_output_format(P, ISL_FORMAT_C);

    PrintGPUUserData Data;
    Data.PPCGProg = PPCGProg;

    auto *Options = isl_ast_print_options_alloc(S->getIslCtx());
    Options =
        isl_ast_print_options_set_print_user(Options, printHostUser, &Data);
    P = isl_ast_node_print(Tree, P, Options);
    char *String = isl_printer_get_str(P);
    printf("# host\n");
    printf("%s\n", String);
    free(String);
    isl_printer_free(P);

    for (auto Kernel : Data.Kernels) {
      printf("# kernel%d\n", Kernel->id);
      printKernel(Kernel);
    }
  }

  // Generate a GPU program using PPCG.
  //
  // GPU mapping consists of multiple steps:
  //
  //  1) Compute new schedule for the program.
  //  2) Map schedule to GPU (TODO)
  //  3) Generate code for new schedule (TODO)
  //
  // We do not use here the Polly ScheduleOptimizer, as the schedule optimizer
  // is mostly CPU specific. Instead, we use PPCG's GPU code generation
  // strategy directly from this pass.
  gpu_gen *generateGPU(ppcg_scop *PPCGScop, gpu_prog *PPCGProg) {

    auto PPCGGen = isl_calloc_type(S->getIslCtx(), struct gpu_gen);

    PPCGGen->ctx = S->getIslCtx();
    PPCGGen->options = PPCGScop->options;
    PPCGGen->print = nullptr;
    PPCGGen->print_user = nullptr;
    PPCGGen->build_ast_expr = &pollyBuildAstExprForStmt;
    PPCGGen->prog = PPCGProg;
    PPCGGen->tree = nullptr;
    PPCGGen->types.n = 0;
    PPCGGen->types.name = nullptr;
    PPCGGen->sizes = nullptr;
    PPCGGen->used_sizes = nullptr;
    PPCGGen->kernel_id = 0;

    // Set scheduling strategy to same strategy PPCG is using.
    isl_options_set_schedule_outer_coincidence(PPCGGen->ctx, true);
    isl_options_set_schedule_maximize_band_depth(PPCGGen->ctx, true);
    isl_options_set_schedule_whole_component(PPCGGen->ctx, false);

    isl_schedule *Schedule = get_schedule(PPCGGen);

    int has_permutable = has_any_permutable_node(Schedule);

    if (!has_permutable || has_permutable < 0) {
      Schedule = isl_schedule_free(Schedule);
    } else {
      Schedule = map_to_device(PPCGGen, Schedule);
      PPCGGen->tree = generate_code(PPCGGen, isl_schedule_copy(Schedule));
    }

    if (DumpSchedule) {
      isl_printer *P = isl_printer_to_str(S->getIslCtx());
      P = isl_printer_set_yaml_style(P, ISL_YAML_STYLE_BLOCK);
      P = isl_printer_print_str(P, "Schedule\n");
      P = isl_printer_print_str(P, "========\n");
      if (Schedule)
        P = isl_printer_print_schedule(P, Schedule);
      else
        P = isl_printer_print_str(P, "No schedule found\n");

      printf("%s\n", isl_printer_get_str(P));
      isl_printer_free(P);
    }

    if (DumpCode) {
      printf("Code\n");
      printf("====\n");
      if (PPCGGen->tree)
        printGPUTree(PPCGGen->tree, PPCGProg);
      else
        printf("No code generated\n");
    }

    isl_schedule_free(Schedule);

    return PPCGGen;
  }

  /// Free gpu_gen structure.
  ///
  /// @param PPCGGen The ppcg_gen object to free.
  void freePPCGGen(gpu_gen *PPCGGen) {
    isl_ast_node_free(PPCGGen->tree);
    isl_union_map_free(PPCGGen->sizes);
    isl_union_map_free(PPCGGen->used_sizes);
    free(PPCGGen);
  }

  /// Free the options in the ppcg scop structure.
  ///
  /// ppcg is not freeing these options for us. To avoid leaks we do this
  /// ourselves.
  ///
  /// @param PPCGScop The scop referencing the options to free.
  void freeOptions(ppcg_scop *PPCGScop) {
    free(PPCGScop->options->debug);
    PPCGScop->options->debug = nullptr;
    free(PPCGScop->options);
    PPCGScop->options = nullptr;
  }

  /// Approximate the number of points in the set.
  ///
  /// This function returns an ast expression that overapproximates the number
  /// of points in an isl set through the rectangular hull surrounding this set.
  ///
  /// @param Set   The set to count.
  /// @param Build The isl ast build object to use for creating the ast
  ///              expression.
  ///
  /// @returns An approximation of the number of points in the set.
  __isl_give isl_ast_expr *approxPointsInSet(__isl_take isl_set *Set,
                                             __isl_keep isl_ast_build *Build) {

    isl_val *One = isl_val_int_from_si(isl_set_get_ctx(Set), 1);
    auto *Expr = isl_ast_expr_from_val(isl_val_copy(One));

    isl_space *Space = isl_set_get_space(Set);
    Space = isl_space_params(Space);
    auto *Univ = isl_set_universe(Space);
    isl_pw_aff *OneAff = isl_pw_aff_val_on_domain(Univ, One);

    for (long i = 0; i < isl_set_dim(Set, isl_dim_set); i++) {
      isl_pw_aff *Max = isl_set_dim_max(isl_set_copy(Set), i);
      isl_pw_aff *Min = isl_set_dim_min(isl_set_copy(Set), i);
      isl_pw_aff *DimSize = isl_pw_aff_sub(Max, Min);
      DimSize = isl_pw_aff_add(DimSize, isl_pw_aff_copy(OneAff));
      auto DimSizeExpr = isl_ast_build_expr_from_pw_aff(Build, DimSize);
      Expr = isl_ast_expr_mul(Expr, DimSizeExpr);
    }

    isl_set_free(Set);
    isl_pw_aff_free(OneAff);

    return Expr;
  }

  /// Approximate a number of dynamic instructions executed by a given
  /// statement.
  ///
  /// @param Stmt  The statement for which to compute the number of dynamic
  ///              instructions.
  /// @param Build The isl ast build object to use for creating the ast
  ///              expression.
  /// @returns An approximation of the number of dynamic instructions executed
  ///          by @p Stmt.
  __isl_give isl_ast_expr *approxDynamicInst(ScopStmt &Stmt,
                                             __isl_keep isl_ast_build *Build) {
    auto Iterations = approxPointsInSet(Stmt.getDomain(), Build);

    long InstCount = 0;

    if (Stmt.isBlockStmt()) {
      auto *BB = Stmt.getBasicBlock();
      InstCount = std::distance(BB->begin(), BB->end());
    } else {
      auto *R = Stmt.getRegion();

      for (auto *BB : R->blocks()) {
        InstCount += std::distance(BB->begin(), BB->end());
      }
    }

    isl_val *InstVal = isl_val_int_from_si(S->getIslCtx(), InstCount);
    auto *InstExpr = isl_ast_expr_from_val(InstVal);
    return isl_ast_expr_mul(InstExpr, Iterations);
  }

  /// Approximate dynamic instructions executed in scop.
  ///
  /// @param S     The scop for which to approximate dynamic instructions.
  /// @param Build The isl ast build object to use for creating the ast
  ///              expression.
  /// @returns An approximation of the number of dynamic instructions executed
  ///          in @p S.
  __isl_give isl_ast_expr *
  getNumberOfIterations(Scop &S, __isl_keep isl_ast_build *Build) {
    isl_ast_expr *Instructions;

    isl_val *Zero = isl_val_int_from_si(S.getIslCtx(), 0);
    Instructions = isl_ast_expr_from_val(Zero);

    for (ScopStmt &Stmt : S) {
      isl_ast_expr *StmtInstructions = approxDynamicInst(Stmt, Build);
      Instructions = isl_ast_expr_add(Instructions, StmtInstructions);
    }
    return Instructions;
  }

  /// Create a check that ensures sufficient compute in scop.
  ///
  /// @param S     The scop for which to ensure sufficient compute.
  /// @param Build The isl ast build object to use for creating the ast
  ///              expression.
  /// @returns An expression that evaluates to TRUE in case of sufficient
  ///          compute and to FALSE, otherwise.
  __isl_give isl_ast_expr *
  createSufficientComputeCheck(Scop &S, __isl_keep isl_ast_build *Build) {
    auto Iterations = getNumberOfIterations(S, Build);
    auto *MinComputeVal = isl_val_int_from_si(S.getIslCtx(), MinCompute);
    auto *MinComputeExpr = isl_ast_expr_from_val(MinComputeVal);
    return isl_ast_expr_ge(Iterations, MinComputeExpr);
  }

  /// Check if the basic block contains a function we cannot codegen for GPU
  /// kernels.
  ///
  /// If this basic block does something with a `Function` other than calling
  /// a function that we support in a kernel, return true.
  bool containsInvalidKernelFunctionInBllock(const BasicBlock *BB) {
    for (const Instruction &Inst : *BB) {
      const CallInst *Call = dyn_cast<CallInst>(&Inst);
      if (Call && isValidFunctionInKernel(Call->getCalledFunction())) {
        continue;
      }

      for (Value *SrcVal : Inst.operands()) {
        PointerType *p = dyn_cast<PointerType>(SrcVal->getType());
        if (!p)
          continue;
        if (isa<FunctionType>(p->getElementType()))
          return true;
      }
    }
    return false;
  }

  /// Return whether the Scop S uses functions in a way that we do not support.
  bool containsInvalidKernelFunction(const Scop &S) {
    for (auto &Stmt : S) {
      if (Stmt.isBlockStmt()) {
        if (containsInvalidKernelFunctionInBllock(Stmt.getBasicBlock()))
          return true;
      } else {
        assert(Stmt.isRegionStmt() &&
               "Stmt was neither block nor region statement");
        for (const BasicBlock *BB : Stmt.getRegion()->blocks())
          if (containsInvalidKernelFunctionInBllock(BB))
            return true;
      }
    }
    return false;
  }

  /// Generate code for a given GPU AST described by @p Root.
  ///
  /// @param Root An isl_ast_node pointing to the root of the GPU AST.
  /// @param Prog The GPU Program to generate code for.
  void generateCode(__isl_take isl_ast_node *Root, gpu_prog *Prog) {
    ScopAnnotator Annotator;
    Annotator.buildAliasScopes(*S);

    Region *R = &S->getRegion();

    simplifyRegion(R, DT, LI, RI);

    BasicBlock *EnteringBB = R->getEnteringBlock();

    PollyIRBuilder Builder = createPollyIRBuilder(EnteringBB, Annotator);

    // Only build the run-time condition and parameters _after_ having
    // introduced the conditional branch. This is important as the conditional
    // branch will guard the original scop from new induction variables that
    // the SCEVExpander may introduce while code generating the parameters and
    // which may introduce scalar dependences that prevent us from correctly
    // code generating this scop.
    BBPair StartExitBlocks =
        executeScopConditionally(*S, Builder.getTrue(), *DT, *RI, *LI);
    BasicBlock *StartBlock = std::get<0>(StartExitBlocks);

    GPUNodeBuilder NodeBuilder(Builder, Annotator, *DL, *LI, *SE, *DT, *S,
                               StartBlock, Prog, Runtime, Architecture);

    // TODO: Handle LICM
    auto SplitBlock = StartBlock->getSinglePredecessor();
    Builder.SetInsertPoint(SplitBlock->getTerminator());
    NodeBuilder.addParameters(S->getContext());

    isl_ast_build *Build = isl_ast_build_alloc(S->getIslCtx());
    isl_ast_expr *Condition = IslAst::buildRunCondition(*S, Build);
    isl_ast_expr *SufficientCompute = createSufficientComputeCheck(*S, Build);
    Condition = isl_ast_expr_and(Condition, SufficientCompute);
    isl_ast_build_free(Build);

    Value *RTC = NodeBuilder.createRTC(Condition);
    Builder.GetInsertBlock()->getTerminator()->setOperand(0, RTC);

    Builder.SetInsertPoint(&*StartBlock->begin());

    NodeBuilder.initializeAfterRTH();
    NodeBuilder.create(Root);
    NodeBuilder.finalize();

    /// In case a sequential kernel has more surrounding loops as any parallel
    /// kernel, the SCoP is probably mostly sequential. Hence, there is no
    /// point in running it on a GPU.
    if (NodeBuilder.DeepestSequential > NodeBuilder.DeepestParallel)
      SplitBlock->getTerminator()->setOperand(0, Builder.getFalse());

    if (!NodeBuilder.BuildSuccessful)
      SplitBlock->getTerminator()->setOperand(0, Builder.getFalse());
  }

  bool runOnScop(Scop &CurrentScop) override {
    S = &CurrentScop;
    LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    DL = &S->getRegion().getEntry()->getModule()->getDataLayout();
    RI = &getAnalysis<RegionInfoPass>().getRegionInfo();

    // We currently do not support functions other than intrinsics inside
    // kernels, as code generation will need to offload function calls to the
    // kernel. This may lead to a kernel trying to call a function on the host.
    // This also allows us to prevent codegen from trying to take the
    // address of an intrinsic function to send to the kernel.
    if (containsInvalidKernelFunction(CurrentScop)) {
      DEBUG(
          dbgs()
              << "Scop contains function which cannot be materialised in a GPU "
                 "kernel. Bailing out.\n";);
      return false;
    }

    auto PPCGScop = createPPCGScop();
    auto PPCGProg = createPPCGProg(PPCGScop);
    auto PPCGGen = generateGPU(PPCGScop, PPCGProg);

    if (PPCGGen->tree) {
      generateCode(isl_ast_node_copy(PPCGGen->tree), PPCGProg);
      CurrentScop.markAsToBeSkipped();
    }

    freeOptions(PPCGScop);
    freePPCGGen(PPCGGen);
    gpu_prog_free(PPCGProg);
    ppcg_scop_free(PPCGScop);

    return true;
  }

  void printScop(raw_ostream &, Scop &) const override {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<RegionInfoPass>();
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addRequired<ScopDetectionWrapperPass>();
    AU.addRequired<ScopInfoRegionPass>();
    AU.addRequired<LoopInfoWrapperPass>();

    AU.addPreserved<AAResultsWrapperPass>();
    AU.addPreserved<BasicAAWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();
    AU.addPreserved<DominatorTreeWrapperPass>();
    AU.addPreserved<GlobalsAAWrapperPass>();
    AU.addPreserved<ScopDetectionWrapperPass>();
    AU.addPreserved<ScalarEvolutionWrapperPass>();
    AU.addPreserved<SCEVAAWrapperPass>();

    // FIXME: We do not yet add regions for the newly generated code to the
    //        region tree.
    AU.addPreserved<RegionInfoPass>();
    AU.addPreserved<ScopInfoRegionPass>();
  }
};
} // namespace

char PPCGCodeGeneration::ID = 1;

Pass *polly::createPPCGCodeGenerationPass(GPUArch Arch, GPURuntime Runtime) {
  PPCGCodeGeneration *generator = new PPCGCodeGeneration();
  generator->Runtime = Runtime;
  generator->Architecture = Arch;
  return generator;
}

INITIALIZE_PASS_BEGIN(PPCGCodeGeneration, "polly-codegen-ppcg",
                      "Polly - Apply PPCG translation to SCOP", false, false)
INITIALIZE_PASS_DEPENDENCY(DependenceInfo);
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass);
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass);
INITIALIZE_PASS_DEPENDENCY(RegionInfoPass);
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass);
INITIALIZE_PASS_DEPENDENCY(ScopDetectionWrapperPass);
INITIALIZE_PASS_END(PPCGCodeGeneration, "polly-codegen-ppcg",
                    "Polly - Apply PPCG translation to SCOP", false, false)
