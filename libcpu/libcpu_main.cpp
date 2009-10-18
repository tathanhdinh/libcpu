/*
 libcpu
 (C)2007-2009 Michael Steil
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* project global headers */
#include "libcpu.h"
#include "tag_generic.h"
#include "disasm.h"
#include "arch.h"

using namespace llvm;
Function* func_jitmain;
Value *ptr_reg;
Value* ptr_PC;
Value* ptr_RAM;
PointerType* type_pfunc_callout;
Value *ptr_func_debug;

/* will hold the system's static memory layout for analysis */
uint8_t *RAM; // XXX global!

typedef enum {				/* bitfield! */
	TYPE_UNKNOWN     = 0,	/* unused or data */
	TYPE_CODE        = 1,	/* identified as code */
	TYPE_CODE_TARGET = 2,	/* static entry address */
	TYPE_AFTER_CALL  = 4,	/* dynamic entry address, will require handling in dynamic entry table */
	TYPE_AFTER_BRANCH= 8,	/* start of basic block, because after branch */
	TYPE_ENTRY       = 16,	/* start of basic block, because after branch */
	TYPE_SUBROUTINE  = 32,	/* CALLs point there */
#ifdef RET_OPTIMIZATION
	TYPE_CALL        = 64,	/* this instruction is a CALL */
	TYPE_SAME_ENTRY  = 128	/* all instructions tagged with this at a time are at the same stack level */
#endif
};

#include "arch/6502/libcpu_6502.h"
#include "arch/mips/libcpu_mips.h"

//////////////////////////////////////////////////////////////////////
// cpu_t
//////////////////////////////////////////////////////////////////////
cpu_t *
cpu_new(cpu_arch_t arch)
{
	cpu_t *cpu;
	
	cpu = (cpu_t*)malloc(sizeof(cpu_t));
	cpu->arch = arch;

	switch (arch) {
		case CPU_ARCH_6502:
			cpu->f = arch_func_6502;
			break;
		case CPU_ARCH_MIPS:
			cpu->f = arch_func_mips;
			break;
		default:
			printf("illegal arch: %d\n", arch);
			exit(1);
	}

	cpu->name = "noname";
	cpu->code_start = 0;
	cpu->code_end = 0;
	cpu->code_entry = 0;
	cpu->tagging_type = NULL;

	cpu->fp = NULL;
	cpu->reg = NULL;
	cpu->mod = new Module(cpu->name);
	cpu->exec_engine = ExecutionEngine::create(cpu->mod);

//	cpu->mod->setDataLayout("e-p:32:32:32-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:32:64-f32:32:32-f64:32:64-v64:64:64-v128:128:128-a0:0:64-f80:32:32");
//	cpu->mod->setTargetTriple("i386-pc-linux-gnu");

	return cpu;
}

void
cpu_set_ram(uint8_t *r)
{
	RAM = r; //XXX global!
}

void
cpu_set_flags_optimize(cpu_t *cpu, uint64_t f)
{
	cpu->flags_optimize = f;
}

void
cpu_set_flags_debug(cpu_t *cpu, uint32_t f)
{
	cpu->flags_debug = f;
}

void
cpu_set_flags_arch(cpu_t *cpu, uint32_t f)
{
	cpu->flags_arch = f;
}

//////////////////////////////////////////////////////////////////////
// disassemble
//////////////////////////////////////////////////////////////////////
void disasm_instr(cpu_t *cpu, addr_t pc) {
	char disassembly_line[MAX_DISASSEMBLY_LINE];
	int bytes, i;

	bytes = cpu->f.disasm_instr(RAM, pc, disassembly_line, sizeof(disassembly_line));

#ifdef DUMP_OCTAL16
	printf(".,%06o ", pc);
	for (i=0; i<bytes; i+=2) {
		printf("%06o ", RAM[pc+i] | RAM[pc+i+1]<<8);
	}
	for (i=0; i<=18-7*(bytes/2); i++) { /* TODO make this arch neutral */
		printf(" ");
	}
#else
	printf(".,%04llx ", (unsigned long long)pc);
	for (i=0; i<bytes; i++) {
		printf("%02X ", RAM[pc+i]);
	}
	for (i=0; i<=18-3*bytes; i++) { /* TODO make this arch neutral */
		printf(" ");
	}
#endif
	printf("%-23s\n", disassembly_line);
}

//////////////////////////////////////////////////////////////////////
// tagging
//////////////////////////////////////////////////////////////////////
static void
init_tagging(cpu_t *cpu)
{
	addr_t tagging_size, i;

	tagging_size = cpu->code_end - cpu->code_start;
	cpu->tagging_type = (tagging_type_t*)malloc(tagging_size);
	for (i = 0; i < tagging_size; i++)
		cpu->tagging_type[i] = TYPE_UNKNOWN;
}

static tagging_type_t
get_tagging_type(cpu_t *cpu, addr_t a) {
	return cpu->tagging_type[a - cpu->code_start];
}

static void
or_tagging_type(cpu_t *cpu, addr_t a, tagging_type_t t) {
	if (a >= cpu->code_start && a <= cpu->code_end)
		cpu->tagging_type[a - cpu->code_start] |= t;
}

static void
tag_recursive(cpu_t *cpu, addr_t pc, int level) {
	int bytes;
	int flow_type;
	addr_t new_pc;

	or_tagging_type(cpu, pc, TYPE_CODE_TARGET); /* someone branches here */

	for(;;) {
		if ((pc < cpu->code_start) || (pc >= cpu->code_end))
			return;

		if (get_tagging_type(cpu, pc) & TYPE_CODE)	/* we have already been here */
			return;

#ifdef VERBOSE
		for (int i=0; i<level; i++) printf(" ");
		disasm_instr(cpu, pc);
#endif

		or_tagging_type(cpu, pc, TYPE_CODE);

		bytes = cpu->f.tag_instr(RAM, pc, &flow_type, &new_pc);
		
		switch (flow_type) {
			case FLOW_TYPE_ERR:
			case FLOW_TYPE_RET:
				return;
			case FLOW_TYPE_JUMP:
				tag_recursive(cpu, new_pc, level+1);
				return;
			case FLOW_TYPE_CALL:
#ifdef RET_OPTIMIZATION
				or_tagging_type(cpu, pc, TYPE_CALL);
#endif
				or_tagging_type(cpu, pc+bytes, TYPE_AFTER_CALL); /* next instruction needs a label */
				if (new_pc != NEW_PC_NONE) {
					//or_tagging_type(cpu, new_pc, TYPE_SUBROUTINE);
					tag_recursive(cpu, new_pc, level+1);
				}
				break;
			case FLOW_TYPE_BRANCH:
				tag_recursive(cpu, new_pc, level+1);
				or_tagging_type(cpu, pc+bytes, TYPE_AFTER_BRANCH); /* next instruction needs a label */
				break;
			case FLOW_TYPE_CONTINUE:
				break; /* continue with next instruction */
		}
		pc += bytes;
	}
}

void
cpu_tag(cpu_t *cpu, addr_t pc) {
	/* for singlestep, we don't need this */
	if (cpu->flags_debug & CPU_DEBUG_SINGLESTEP)
		return;

	/* initialize data structure on demand */
	if (!cpu->tagging_type)
		init_tagging(cpu);

#if VERBOSE
	printf("starting tagging at $%02llx\n", (unsigned long long)pc);
#endif

	or_tagging_type(cpu, pc, TYPE_ENTRY); /* add dispatch entry */
	tag_recursive(cpu, pc, 0);
}

//////////////////////////////////////////////////////////////////////
// generic code
//////////////////////////////////////////////////////////////////////
#define LABEL_PREFIX 'L'

const BasicBlock *
lookup_basicblock(Function* f, addr_t pc) {
	Function::const_iterator it;
	for (it = f->getBasicBlockList().begin(); it != f->getBasicBlockList().end(); it++) {
		const char *cstr = (*it).getNameStr().c_str();
		if (cstr[0] == LABEL_PREFIX) {
			addr_t pc2 = strtol(cstr + 1, (char **)NULL, 16);
			if (pc == pc2)
				return it;
		}
	}
	printf("error: basic block 0x%llx not found!\n", pc);
	return NULL;
}

void
create_call(Value *ptr_fp, BasicBlock *bb) {
	
	std::vector<Value*> void_49_params;
	void_49_params.push_back(ptr_RAM);
	void_49_params.push_back(ptr_reg);
	CallInst* void_49 = CallInst::Create(ptr_fp, void_49_params.begin(), void_49_params.end(), "", bb);
	void_49->setCallingConv(CallingConv::C);
	void_49->setTailCall(false);
	AttrListPtr void_49_PAL;
	{
		SmallVector<AttributeWithIndex, 4> Attrs;
		AttributeWithIndex PAWI;
		PAWI.Index = 4294967295U; PAWI.Attrs = 0 | Attribute::NoUnwind;
		Attrs.push_back(PAWI);
		void_49_PAL = AttrListPtr::get(Attrs.begin(), Attrs.end());
	}
	void_49->setAttributes(void_49_PAL);
}

Value *
get_struct_member_pointer(Value *s, int index, BasicBlock *bb) {
	ConstantInt* const_0 = ConstantInt::get(Type::Int32Ty, 0);
	ConstantInt* const_index = ConstantInt::get(Type::Int32Ty, index);

	std::vector<Value*> ptr_11_indices;
	ptr_11_indices.push_back(const_0);
	ptr_11_indices.push_back(const_index);
	return (Value*) GetElementPtrInst::Create(s, ptr_11_indices.begin(), ptr_11_indices.end(), "", bb);
}

//////////////////////////////////////////////////////////////////////
// optimize
//////////////////////////////////////////////////////////////////////
void
optimize(cpu_t *cpu) {
	PassManager pm;
	uint64_t flags = cpu->flags_optimize;

	pm.add(new TargetData(cpu->mod));

	//pm.add(createStripDeadPrototypesPass());
	if (flags & (1ULL<<0))
		pm.add(createGlobalDCEPass());

	if (flags & (1ULL<<1))
		pm.add(createRaiseAllocationsPass());
	if (flags & (1ULL<<2))
		pm.add(createCFGSimplificationPass());
	if (flags & (1ULL<<3))
		pm.add(createPromoteMemoryToRegisterPass());
	if (flags & (1ULL<<4))
		pm.add(createGlobalOptimizerPass());
	if (flags & (1ULL<<5))
		pm.add(createGlobalDCEPass());

	if (flags & (1ULL<<6))
		pm.add(createIPConstantPropagationPass());
	if (flags & (1ULL<<7))
		pm.add(createDeadArgEliminationPass());
	if (flags & (1ULL<<8))
		pm.add(createInstructionCombiningPass());
	if (flags & (1ULL<<9))
		pm.add(createCFGSimplificationPass());
	if (flags & (1ULL<<10))
		pm.add(createPruneEHPass());

	if (flags & (1ULL<<11))
		pm.add(createFunctionInliningPass());

	if (flags & (1ULL<<12))
		pm.add(createArgumentPromotionPass());
	if (flags & (1ULL<<13))
		pm.add(createTailDuplicationPass());
	if (flags & (1ULL<<14))
		pm.add(createInstructionCombiningPass());
	if (flags & (1ULL<<15))
		pm.add(createCFGSimplificationPass());
	if (flags & (1ULL<<16))
		pm.add(createScalarReplAggregatesPass());
	if (flags & (1ULL<<17))
		pm.add(createInstructionCombiningPass());
	if (flags & (1ULL<<18))
		pm.add(createCondPropagationPass());

	if (flags & (1ULL<<19))
		pm.add(createTailCallEliminationPass());
	if (flags & (1ULL<<20))
		pm.add(createCFGSimplificationPass());
	if (flags & (1ULL<<21))
		pm.add(createReassociatePass());
	if (flags & (1ULL<<22))
		pm.add(createLoopRotatePass());
	if (flags & (1ULL<<23))
		pm.add(createLICMPass());
	if (flags & (1ULL<<24))
		pm.add(createLoopUnswitchPass());
	if (flags & (1ULL<<25))
		pm.add(createInstructionCombiningPass());
	if (flags & (1ULL<<26))
		pm.add(createIndVarSimplifyPass());
	if (flags & (1ULL<<27))
		pm.add(createLoopUnrollPass());
	if (flags & (1ULL<<28))
		pm.add(createInstructionCombiningPass());
	if (flags & (1ULL<<29))
		pm.add(createGVNPass());
	if (flags & (1ULL<<30))
		pm.add(createSCCPPass());

	if (flags & (1ULL<<31))
		pm.add(createInstructionCombiningPass());
	if (flags & (1ULL<<32))
		pm.add(createCondPropagationPass());

	if (flags & (1ULL<<33))
		pm.add(createDeadStoreEliminationPass());
	if (flags & (1ULL<<34))
		pm.add(createAggressiveDCEPass());
	if (flags & (1ULL<<35))
		pm.add(createCFGSimplificationPass());
	if (flags & (1ULL<<36))
		pm.add(createSimplifyLibCallsPass());
	if (flags & (1ULL<<37))
		pm.add(createDeadTypeEliminationPass());
	if (flags & (1ULL<<38))
		pm.add(createConstantMergePass());

	pm.run(*cpu->mod);
}

BasicBlock *
create_basicblock(addr_t addr, Function *f) {
	char label[17];
	snprintf(label, sizeof(label), "%c%08llx", LABEL_PREFIX, (unsigned long long)addr);
    return BasicBlock::Create(label, f, 0);
}

static BasicBlock *
cpu_recompile(cpu_t *cpu, BasicBlock *bb_ret)
{
	// find all instructions that need labels and create basic blocks for them
	int bbs = 0;
	addr_t pc, bytes;
	pc = cpu->code_start;
	while (pc<cpu->code_end) {
		//printf("%04X: %d\n", pc, get_tagging_type(cpu, pc));
		if (get_tagging_type(cpu, pc) & (TYPE_CODE_TARGET|TYPE_ENTRY|TYPE_AFTER_CALL|TYPE_AFTER_BRANCH)) {
			create_basicblock(pc, func_jitmain);
			bbs++;
		}

		pc++;
	}
	printf("bbs: %d\n", bbs);

	// create dispatch basicblock
	BasicBlock* bb_dispatch = BasicBlock::Create("dispatch",func_jitmain,0);  
	Value *v_pc = new LoadInst(ptr_PC, "", false, bb_dispatch);
    SwitchInst* sw = SwitchInst::Create(v_pc, bb_ret, bbs /*XXX wrong!*/, bb_dispatch);

	for (pc = cpu->code_start; pc<cpu->code_end; pc++) {
		if (get_tagging_type(cpu, pc) & (TYPE_ENTRY|TYPE_AFTER_CALL)) {
printf("info: adding case: %llx\n", pc);
			ConstantInt* c = ConstantInt::get(IntegerType::get(cpu->pc_width), pc);
			BasicBlock *target = (BasicBlock*)lookup_basicblock(func_jitmain, pc);
			if (!target) {
				printf("error: unknown rts target $%04llx!\n", (unsigned long long)pc);
				exit(1);
			} else {
				sw->addCase(c, target);
			}
		}
	}

// recompile basic blocks
    Function::const_iterator it;
    for (it = func_jitmain->getBasicBlockList().begin(); it != func_jitmain->getBasicBlockList().end(); it++) {
		const BasicBlock *hack = it;
		BasicBlock *cur_bb = (BasicBlock*)hack;
		const char *cstr = (*it).getNameStr().c_str();
		if (cstr[0] != LABEL_PREFIX)
			continue; // skip special blocks like entry, dispatch...
		pc = strtol(cstr+1, (char **)NULL, 16);
printf("basicblock: %04llx\n", (unsigned long long)pc);
		addr_t last_pc;
		do {
			disasm_instr(cpu, pc);

			bytes = cpu->f.recompile_instr(RAM, pc, bb_dispatch, cur_bb);
//printf("bytes: %d\n", bytes);

			last_pc = pc;
			pc += bytes;
		} while ((pc<cpu->code_end) &&
			 (get_tagging_type(cpu, pc) & TYPE_CODE) &&
			 !(get_tagging_type(cpu, pc) & (TYPE_CODE_TARGET|TYPE_ENTRY|TYPE_AFTER_CALL|TYPE_AFTER_BRANCH)));
		// link with next basic block if there isn't a control flow instr. already
		addr_t dummy2;
		int flow_type;
		cpu->f.tag_instr(RAM, last_pc, &flow_type, &dummy2);
		if (flow_type == FLOW_TYPE_CONTINUE) {
			BasicBlock *target = (BasicBlock*)lookup_basicblock(func_jitmain, pc);
			if (!target) {
				printf("error: unknown continue $%04llx!\n", (unsigned long long)pc);
				exit(1);
			}
			printf("info: linking continue $%04llx!\n", (unsigned long long)pc);
			BranchInst::Create(target, (BasicBlock*)cur_bb);
		}
    }

	return bb_dispatch;
}

void
emit_store_pc(cpu_t *cpu, BasicBlock *bb_branch, addr_t new_pc)
{
	Value *v_pc = ConstantInt::get(IntegerType::get(cpu->pc_width), new_pc);
	new StoreInst(v_pc, ptr_PC, bb_branch);
}

void
emit_store_pc_return(cpu_t *cpu, BasicBlock *bb_branch, addr_t new_pc, BasicBlock *bb_ret)
{
	emit_store_pc(cpu, bb_branch, new_pc);
	BranchInst::Create(bb_ret, bb_branch);
}

void
create_singlestep_return_basicblock(cpu_t *cpu, addr_t new_pc, BasicBlock *bb_ret)
{
	BasicBlock *bb_branch = create_basicblock(new_pc, cpu->func_jitmain);
	emit_store_pc_return(cpu, bb_branch, new_pc, bb_ret);
}

static BasicBlock *
cpu_recompile_singlestep(cpu_t *cpu, BasicBlock *bb_ret)
{
	int bytes;
	addr_t new_pc1;
	int flow_type;
	addr_t pc = cpu->f.get_pc(cpu->reg);

	BasicBlock *cur_bb = BasicBlock::Create("instruction", func_jitmain, 0);

	disasm_instr(cpu, pc);

printf("%s:%d\n", __func__, __LINE__);
	bytes = cpu->f.tag_instr(RAM, pc, &flow_type, &new_pc1);

	/* Create two "return" BBs for the branch targets */
	if (flow_type == FLOW_TYPE_BRANCH) {
printf("%s:%d\n", __func__, __LINE__);
		create_singlestep_return_basicblock(cpu, new_pc1, bb_ret);
		create_singlestep_return_basicblock(cpu, pc+bytes, bb_ret);
	}
	/* Create one "return" BB for the jump target */
	if (flow_type == FLOW_TYPE_JUMP || flow_type == FLOW_TYPE_CALL)
		create_singlestep_return_basicblock(cpu, new_pc1, bb_ret);
#if 0
	/* If it's a call, "store PC" (will return anyway) */
	if (flow_type == FLOW_TYPE_CALL){
printf("%s:%d\n", __func__, __LINE__);
		emit_store_pc(cpu, cur_bb, new_pc1);
}
#endif
	bytes = cpu->f.recompile_instr(RAM, pc, bb_ret, cur_bb);

	/* If it's not a branch, append "store PC & return" to basic block */
	if (flow_type == FLOW_TYPE_CONTINUE ) {
		emit_store_pc_return(cpu, cur_bb, pc + bytes, bb_ret);
	}
	return cur_bb;
}

static Function*
cpu_create_function(cpu_t *cpu)
{
	Function *func_jitmain;

	// Type Definitions
	// - struct reg
	StructType *type_struct_reg_t = cpu->f.get_struct_reg();
	cpu->mod->addTypeName("struct.reg_t", type_struct_reg_t);
	// - struct reg *
	PointerType *type_pstruct_reg_t = PointerType::get(type_struct_reg_t, 0);
	// - uint8_t *
	PointerType *type_pi8 = PointerType::get(IntegerType::get(8), 0);
	// - (*f)(uint8_t *, reg_t *) [debug_function() function pointer]
	std::vector<const Type*>type_func_callout_args;
	type_func_callout_args.push_back(type_pi8);				/* uint8_t *RAM */
	type_func_callout_args.push_back(type_pstruct_reg_t);	/* reg_t *reg */
	FunctionType *type_func_callout = FunctionType::get(
		Type::VoidTy,			/* Result */
		type_func_callout_args,	/* Params */
		false);					/* isVarArg */
	type_pfunc_callout = PointerType::get(type_func_callout, 0);

	// - (*f)(uint8_t *, reg_t *, (*)(...)) [jitmain() function pointer)
	std::vector<const Type*>type_func_jitmain_args;
	type_func_jitmain_args.push_back(type_pi8);				/* uint8_t *RAM */
	type_func_jitmain_args.push_back(type_pstruct_reg_t);	/* reg_t *reg */
	type_func_jitmain_args.push_back(type_pfunc_callout);	/* (*debug)(...) */
	FunctionType* type_func_jitmain = FunctionType::get(
		IntegerType::get(32),	/* Result */
		type_func_jitmain_args,		/* Params */
		false);						/* isVarArg */

	// Function Declarations
	func_jitmain = Function::Create(
		type_func_jitmain,				/* Type */
		GlobalValue::ExternalLinkage,	/* Linkage */
		"jitmain", cpu->mod);				/* Name */
	func_jitmain->setCallingConv(CallingConv::C);
	AttrListPtr func_jitmain_PAL;
	{
		SmallVector<AttributeWithIndex, 4> Attrs;
		AttributeWithIndex PAWI;
		PAWI.Index = 1U; PAWI.Attrs = 0  | Attribute::NoCapture;
		Attrs.push_back(PAWI);
		PAWI.Index = 4294967295U; PAWI.Attrs = 0  | Attribute::NoUnwind;
		Attrs.push_back(PAWI);
		func_jitmain_PAL = AttrListPtr::get(Attrs.begin(), Attrs.end());
	}
	func_jitmain->setAttributes(func_jitmain_PAL);

	return func_jitmain;
}

static void
cpu_recompile_function(cpu_t *cpu)
{
	func_jitmain = cpu_create_function(cpu);
	cpu->func_jitmain = func_jitmain;

	// args
	Function::arg_iterator args = func_jitmain->arg_begin();
	ptr_RAM = args++;
	ptr_RAM->setName("RAM");
	ptr_reg = args++;
	ptr_reg->setName("reg");	
    ptr_func_debug = args++;
    ptr_func_debug->setName("debug");

	// entry basicblock
	BasicBlock *label_entry = BasicBlock::Create("entry",func_jitmain,0);
	cpu->f.emit_decode_reg(label_entry);

#if 0//HACK: RAM is at 0
	PointerType* PointerTy_3 = PointerType::get(IntegerType::get(8), 0);
	ptr_RAM = ConstantPointerNull::get(PointerTy_3);
#endif

	// create ret basicblock
	BasicBlock *bb_ret = BasicBlock::Create("ret",func_jitmain,0);  
	cpu->f.spill_reg_state(bb_ret);
	ReturnInst::Create(ConstantInt::get(Type::Int32Ty, JIT_RETURN_FUNCNOTFOUND), bb_ret);

	BasicBlock *bb_start;
	if (cpu->flags_debug & CPU_DEBUG_SINGLESTEP) {
		bb_start = cpu_recompile_singlestep(cpu, bb_ret);
	} else {
		bb_start = cpu_recompile(cpu, bb_ret);
	}

	// entry basicblock
	BranchInst::Create(bb_start, label_entry);

	// make sure everything is OK
	verifyModule(*cpu->mod, PrintMessageAction);

	if (cpu->flags_debug & CPU_DEBUG_PRINT_IR)
		cpu->mod->dump();

	if (cpu->flags_optimize != CPU_OPTIMIZE_NONE) {
		printf("*** Optimizing...");
		optimize(cpu);
		printf("done.\n");
		if (cpu->flags_debug & CPU_DEBUG_PRINT_IR_OPTIMIZED)
			cpu->mod->dump();
	}

	printf("*** Recompiling...");
	cpu->fp = cpu->exec_engine->getPointerToFunction(func_jitmain);
	printf("done.\n");
}

void
cpu_init(cpu_t *cpu)
{
	cpu->f.init(cpu);
}

int
cpu_run(cpu_t *cpu, debug_function_t debug_function)
{
	/* lazy init of frontend */
	if (!cpu->reg)
		cpu_init(cpu);

	/* on demand recompilation */
	if (!cpu->fp)
		cpu_recompile_function(cpu);

	/* run it ! */
	typedef int (*fp_t)(uint8_t *RAM, void *reg, debug_function_t fp);
	fp_t FP = (fp_t)cpu->fp;

	return FP(RAM, cpu->reg, debug_function);
}

void
cpu_flush(cpu_t *cpu)
{
	cpu->exec_engine->freeMachineCodeForFunction(cpu->func_jitmain);
	cpu->func_jitmain->eraseFromParent();

	cpu->fp = 0;

//	delete cpu->mod;
//	cpu->mod = NULL;
}
//printf("%s:%d\n", __func__, __LINE__);