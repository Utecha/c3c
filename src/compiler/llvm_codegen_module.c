// Copyright (c) 2019 Christoffer Lerno. All rights reserved.
// Use of this source code is governed by the GNU LGPLv3.0 license
// a copy of which can be found in the LICENSE file.

#include "llvm_codegen_internal.h"


/**
 * Create the introspection type { byte kind, void* dtable, usz sizeof, typeid inner, usz len, [0xtypeid] additional }
 *
 * @param c the context
 * @return the generated introspection type.
 */
static inline LLVMTypeRef create_introspection_type(GenContext *c)
{
	LLVMTypeRef type = LLVMStructCreateNamed(c->context, ".introspect");
	LLVMTypeRef introspect_type[INTROSPECT_INDEX_TOTAL] = {
			[INTROSPECT_INDEX_KIND] = c->byte_type,
			[INTROSPECT_INDEX_PARENTOF] = c->typeid_type,
			[INTROSPECT_INDEX_DTABLE] = c->ptr_type,
			[INTROSPECT_INDEX_SIZEOF] = c->size_type,
			[INTROSPECT_INDEX_INNER] = c->typeid_type,
			[INTROSPECT_INDEX_LEN] = c->size_type,
			[INTROSPECT_INDEX_ADDITIONAL] = LLVMArrayType(c->typeid_type, 0),
	};
	LLVMStructSetBody(type, introspect_type, INTROSPECT_INDEX_TOTAL, false);
	return type;
}

/**
 * Create the fault type { typeid parent, String[] name, usz ordinal }
 * @param c the context to use.
 * @return the resulting LLVM type.
 */
static inline LLVMTypeRef create_fault_type(GenContext *c)
{
	LLVMTypeRef type = LLVMStructCreateNamed(c->context, ".fault");
	LLVMTypeRef fault_type[] = { c->typeid_type, c->chars_type, c->size_type };
	LLVMStructSetBody(type, fault_type, 3, false);
	return type;
}

/**
 * Set a module flag.
 *
 * @param c the context to use
 * @param flag_behavior how the flag should be merged
 * @param flag the flag name
 * @param value the flag value
 * @param type the type of the flag value
 */
static void llvm_set_module_flag(GenContext *c, LLVMModuleFlagBehavior flag_behavior, const char *flag, uint64_t value, Type *type)
{
	LLVMMetadataRef val = LLVMValueAsMetadata(LLVMConstInt(LLVMIntTypeInContext(c->context, type_bit_size(type)), value, false));
	LLVMAddModuleFlag(c->module, flag_behavior, flag, strlen(flag), val);
}

void gencontext_begin_module(GenContext *c)
{
	ASSERT(!c->module && "Expected no module");

	codegen_setup_object_names(c->code_module, &c->base_name, &c->ir_filename, &c->asm_filename, &c->object_filename);
	DEBUG_LOG("Emit module %s.", c->code_module->name->module);
	c->panic_var = compiler.context.panic_var;
	c->panicf = compiler.context.panicf;
	c->module = LLVMModuleCreateWithNameInContext(c->code_module->name->module, c->context);
	c->machine = llvm_target_machine_create();

	c->target_data = LLVMCreateTargetDataLayout(c->machine);

	LLVMSetModuleDataLayout(c->module, c->target_data);
	LLVMSetSourceFileName(c->module, c->code_module->name->module, strlen(c->code_module->name->module));

	LLVMSetTarget(c->module, compiler.platform.target_triple);

	// Setup all types. Not thread-safe, but at this point in time we can assume a single context.
	// We need to remove the context from the cache after this.
	// This would seem to indicate that we should change Type / actual type.

	c->ast_alloca_addr_space = compiler.platform.alloca_address_space;
	FOREACH(Type *, type, compiler.context.type)
	{
		type->backend_type = NULL;
		type->backend_debug_type = NULL;
		type->backend_typeid = NULL;
		switch (type->type_kind)
		{
			case TYPE_ENUM:
			{
				FOREACH(Decl *, value, type->decl->enums.values)
				{
					value->backend_ref = NULL;
				}
				FALLTHROUGH;
			}
			case TYPE_STRUCT:
			case TYPE_UNION:
			case TYPE_DISTINCT:
				type->decl->backend_ref = NULL;
				break;
			case TYPE_FUNC_RAW:
				//REMINDER("Clear func when it has reflection");
				break;
			default:
				break;
		}
	}
	c->bool_type = LLVMInt1TypeInContext(c->context);
	c->byte_type = LLVMInt8TypeInContext(c->context);
	c->ptr_type = LLVMPointerType(c->byte_type, 0);
	c->size_type = llvm_get_type(c, type_usz);
	c->typeid_type = llvm_get_type(c, type_typeid);
	LLVMTypeRef void_type = LLVMVoidTypeInContext(c->context);
	LLVMTypeRef dtable_type[3] = { c->ptr_type, c->ptr_type, c->ptr_type };
	c->dtable_type = LLVMStructTypeInContext(c->context, dtable_type, 3, false);
	c->chars_type = llvm_get_type(c, type_chars);
	LLVMTypeRef ctor_type[3] = { LLVMInt32TypeInContext(c->context), c->ptr_type, c->ptr_type };
	c->xtor_entry_type = LLVMStructTypeInContext(c->context, ctor_type, 3, false);
	c->xtor_func_type = LLVMFunctionType(void_type, NULL, 0, false);
	c->introspect_type = create_introspection_type(c);
	c->atexit_type = LLVMFunctionType(void_type, &c->ptr_type, 1, false);
	c->fault_type = create_fault_type(c);
	c->memcmp_function = NULL;
	LLVMTypeRef memcmp_types[3] = {c->ptr_type, c->ptr_type, c->size_type };
	c->memcmp_function_type = LLVMFunctionType(llvm_get_type(c, type_cint), memcmp_types, 3, false);
	if (c->panic_var) c->panic_var->backend_ref = NULL;
	if (c->panicf) c->panicf->backend_ref = NULL;
	bool is_win = compiler.build.arch_os_target == WINDOWS_X64 || compiler.build.arch_os_target == WINDOWS_AARCH64;
	bool is_codeview = compiler.build.feature.win_debug != WIN_DEBUG_DWARF && is_win;
	if (is_codeview)
	{
		llvm_set_module_flag(c, LLVMModuleFlagBehaviorError, "CodeView", 1, type_uint);
	}
	else
	{
		llvm_set_module_flag(c, LLVMModuleFlagBehaviorWarning, "Dwarf Version", 4, type_uint);
	}
	llvm_set_module_flag(c, LLVMModuleFlagBehaviorWarning, "Debug Info Version", 3, type_uint);
	llvm_set_module_flag(c, LLVMModuleFlagBehaviorWarning, "wchar_size", is_win ? 2 : 4, type_uint);
	static const char *pic_level = "PIC Level";
	static const char *pie_level = "PIE Level";
	switch (compiler.platform.reloc_model)
	{
		case RELOC_BIG_PIE:
			llvm_set_module_flag(c, LLVMModuleFlagBehaviorOverride, pie_level, (unsigned)2 /* PIE */, type_uint);
			FALLTHROUGH;
		case RELOC_BIG_PIC:
			llvm_set_module_flag(c, LLVMModuleFlagBehaviorOverride, pic_level, (unsigned)2 /* PIC */, type_uint);
			break;
		case RELOC_SMALL_PIE:
			llvm_set_module_flag(c, LLVMModuleFlagBehaviorOverride, pie_level, (unsigned)1 /* pie */, type_uint);
			FALLTHROUGH;
		case RELOC_SMALL_PIC:
			llvm_set_module_flag(c, LLVMModuleFlagBehaviorOverride, pic_level, (unsigned)1 /* pic */, type_uint);
			break;
		default:
			break;
	}
	llvm_set_module_flag(c, LLVMModuleFlagBehaviorError, "uwtable", UWTABLE, type_uint);
	if (is_win)
	{
		llvm_set_module_flag(c, LLVMModuleFlagBehaviorError, "MaxTLSAlign", 65536, type_uint);
	}
	else
	{
		unsigned frame_pointer = compiler.platform.arch == ARCH_TYPE_AARCH64 ? 1 : 2;
		llvm_set_module_flag(c, LLVMModuleFlagBehaviorWarning, "frame-pointer", frame_pointer, type_uint);
	}

	if (compiler.build.debug_info != DEBUG_INFO_NONE)
	{
		c->debug.runtime_version = 0;
		c->debug.builder = LLVMCreateDIBuilder(c->module);
		if (compiler.build.debug_info == DEBUG_INFO_FULL && safe_mode_enabled())
		{
			c->debug.enable_stacktrace = os_supports_stacktrace(compiler.platform.os);
		}
		c->debug.emit_expr_loc = !is_win;
	}
	c->global_builder = LLVMCreateBuilder();
	c->builder = c->global_builder;
}

void gencontext_init_file_emit(GenContext *c, CompilationUnit *unit)
{
	if (compiler.build.debug_info != DEBUG_INFO_NONE)
	{
		// Set runtime version here.
		unit->llvm.debug_file = llvm_get_debug_file(c, unit->file->file_id);

		bool is_optimized = compiler.build.optlevel != OPTIMIZATION_NONE;
		const char *dwarf_flags = "";
		unsigned runtime_version = 0;
		LLVMDWARFEmissionKind emission_kind =
				compiler.build.debug_info == DEBUG_INFO_FULL ? LLVMDWARFEmissionFull : LLVMDWARFEmissionLineTablesOnly;
		const char *debug_output_file = "";
		bool emit_debug_info_for_profiling = false;
		bool split_debug_inlining = false;
		const char *sysroot = "";
		const char *sdk = "";
		unsigned dwo_id = 0;
		if (c->debug.compile_unit)
		{
			unit->llvm.debug_compile_unit = c->debug.compile_unit;
			return;
		}
		unit->llvm.debug_compile_unit = LLVMDIBuilderCreateCompileUnit(c->debug.builder,
																	   LLVMDWARFSourceLanguageC11,
																	   unit->llvm.debug_file,
																	   DWARF_PRODUCER_NAME,
																	   strlen(DWARF_PRODUCER_NAME),
																	   is_optimized,
																	   dwarf_flags,
																	   strlen(dwarf_flags),
																	   runtime_version,
																	   debug_output_file,
																	   strlen(debug_output_file),
																	   emission_kind,
																	   dwo_id,
																	   split_debug_inlining,
																	   emit_debug_info_for_profiling,
																	   sysroot,
																	   strlen(sysroot),
																	   sdk,
																	   strlen(sdk)
																	  );

	}
}


void gencontext_end_file_emit(GenContext *c UNUSED, CompilationUnit *ast UNUSED)
{
}

void gencontext_end_module(GenContext *context)
{
	LLVMDisposeModule(context->module);
}
