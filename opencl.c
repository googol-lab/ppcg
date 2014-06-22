/*
 * Copyright 2013      Ecole Normale Superieure
 *
 * Use of this software is governed by the GNU LGPLv2.1 license
 *
 * Written by Sven Verdoolaege and Riyadh Baghdadi,
 * Ecole Normale Superieure, 45 rue d’Ulm, 75230 Paris, France
 */

#include <ctype.h>
#include <limits.h>
#include <string.h>

#include <isl/aff.h>
#include <isl/ast.h>

#include "opencl.h"
#include "gpu_print.h"
#include "gpu.h"
#include "ppcg.h"
#include "print.h"
#include "schedule.h"

#define min(a, b)  (((a) < (b)) ? (a) : (b))
#define max(a, b)  (((a) > (b)) ? (a) : (b))

/* options are the global options passed to generate_opencl.
 * input is the name of the input file.
 * output is the user-specified output file name and may be NULL
 *	if not specified by the user.
 * kernel_c_name is the name of the kernel_c file.
 * host_c is the generated source file for the host code.  kernel_c is
 * the generated source file for the kernel.  kernel_h is the generated
 * header file for the kernel.
 */
struct opencl_info {
	struct ppcg_options *options;
	const char *input;
	const char *output;
	char kernel_c_name[PATH_MAX];

	FILE *host_c;
	FILE *kernel_c;
	FILE *kernel_h;
};

/* Open the file called "name" for writing or print an error message.
 */
static FILE *open_or_croak(const char *name)
{
	FILE *file;

	file = fopen(name, "w");
	if (!file)
		fprintf(stderr, "Failed to open \"%s\" for writing\n", name);
	return file;
}

/* Open the host .c file and the kernel .h and .cl files for writing.
 * Their names are derived from info->output (or info->input if
 * the user did not specify an output file name).
 * Add the necessary includes to these files.
 *
 * Return 0 on success and -1 on failure.
 */
static int opencl_open_files(struct opencl_info *info)
{
	char name[PATH_MAX];
	int len;

	if (info->output) {
		const char *ext;

		ext = strrchr(info->output, '.');
		len = ext ? ext - info->output : strlen(info->output);
		memcpy(name, info->output, len);

		info->host_c = open_or_croak(info->output);
	} else {
		len = ppcg_extract_base_name(name, info->input);

		strcpy(name + len, "_host.c");
		info->host_c = open_or_croak(name);
	}

	memcpy(info->kernel_c_name, name, len);
	strcpy(info->kernel_c_name + len, "_kernel.cl");
	info->kernel_c = open_or_croak(info->kernel_c_name);

	strcpy(name + len, "_kernel.h");
	info->kernel_h = open_or_croak(name);

	if (!info->host_c || !info->kernel_c || !info->host_c)
		return -1;

	fprintf(info->host_c, "#include <assert.h>\n");
	fprintf(info->host_c, "#include <stdio.h>\n");
	fprintf(info->host_c, "#include \"%s\"\n\n", ppcg_base_name(name));
	fprintf(info->kernel_h, "#if defined(__APPLE__)\n");
	fprintf(info->kernel_h, "#include <OpenCL/opencl.h>\n");
	fprintf(info->kernel_h, "#else\n");
	fprintf(info->kernel_h, "#include <CL/opencl.h>\n");
	fprintf(info->kernel_h, "#endif\n\n");
	fprintf(info->kernel_h, "cl_device_id opencl_create_device("
				"int use_gpu);\n");
	fprintf(info->kernel_h, "cl_program opencl_build_program("
				"cl_context ctx, "
				"cl_device_id dev, const char *filename, "
				"const char *opencl_options);\n");
	fprintf(info->kernel_h,
		"const char *opencl_error_string(cl_int error);\n");

	return 0;
}

/* Close all output files.
 */
static void opencl_close_files(struct opencl_info *info)
{
	if (info->kernel_c)
		fclose(info->kernel_c);
	if (info->kernel_h)
		fclose(info->kernel_h);
	if (info->host_c)
		fclose(info->host_c);
}

static __isl_give isl_printer *opencl_print_host_macros(__isl_take isl_printer *p)
{
	const char *macros =
		"#define openclCheckReturn(ret) \\\n"
		"  if (ret != CL_SUCCESS) {\\\n"
		"    fprintf(stderr, \"OpenCL error: %s\\n\", "
		"opencl_error_string(ret)); \\\n"
		"    fflush(stderr); \\\n"
		"    assert(ret == CL_SUCCESS);\\\n  }\n";

	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, macros);
	p = isl_printer_end_line(p);

	p = isl_ast_op_type_print_macro(isl_ast_op_max, p);

	return p;
}

static __isl_give isl_printer *opencl_declare_device_arrays(
	__isl_take isl_printer *p, struct gpu_prog *prog)
{
	int i;

	for (i = 0; i < prog->n_array; ++i) {
		if (gpu_array_is_read_only_scalar(&prog->array[i]))
			continue;
		p = isl_printer_start_line(p);
		p = isl_printer_print_str(p, "cl_mem dev_");
		p = isl_printer_print_str(p, prog->array[i].name);
		p = isl_printer_print_str(p, ";");
		p = isl_printer_end_line(p);
	}
	p = isl_printer_start_line(p);
	p = isl_printer_end_line(p);
	return p;
}

/* Given an array, check whether its positive size guard expression is
 * trivial.
 */
static int is_array_positive_size_guard_trivial(struct gpu_array_info *array)
{
	isl_set *guard;
	int is_trivial;

	guard = gpu_array_positive_size_guard(array);
	is_trivial = isl_set_plain_is_universe(guard);
	isl_set_free(guard);
	return is_trivial;
}

/* Allocate a device array for array and copy the contents to the device
 * if copy is set.
 *
 * Emit a max-expression to ensure the device array can contain at least one
 * element if the array's positive size guard expression is not trivial.
 */
static __isl_give isl_printer *allocate_device_array(__isl_take isl_printer *p,
	struct gpu_array_info *array, int copy)
{
	int need_lower_bound;

	p = ppcg_start_block(p);

	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "dev_");
	p = isl_printer_print_str(p, array->name);
	p = isl_printer_print_str(p, " = clCreateBuffer(context, ");
	p = isl_printer_print_str(p, "CL_MEM_READ_WRITE");

	if (!copy)
		p = isl_printer_print_str(p, ", ");
	else
		p = isl_printer_print_str(p, " | CL_MEM_COPY_HOST_PTR, ");

	need_lower_bound = !is_array_positive_size_guard_trivial(array);
	if (need_lower_bound) {
		p = isl_printer_print_str(p, "max(sizeof(");
		p = isl_printer_print_str(p, array->type);
		p = isl_printer_print_str(p, "), ");
	}
	p = gpu_array_info_print_size(p, array);
	if (need_lower_bound)
		p = isl_printer_print_str(p, ")");

	if (!copy)
		p = isl_printer_print_str(p, ", NULL");
	else if (gpu_array_is_scalar(array)) {
		p = isl_printer_print_str(p, ", &");
		p = isl_printer_print_str(p, array->name);
	} else {
		p = isl_printer_print_str(p, ", ");
		p = isl_printer_print_str(p, array->name);
	}

	p = isl_printer_print_str(p, ", &err);");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "openclCheckReturn(err);");
	p = isl_printer_end_line(p);

	p = ppcg_end_block(p);

	return p;
}

/* Allocate device arrays and copy the contents of copy_in arrays into device.
 */
static __isl_give isl_printer *opencl_allocate_device_arrays(
	__isl_take isl_printer *p, struct gpu_prog *prog)
{
	int i, j;

	for (i = 0; i < prog->n_array; ++i) {
		struct gpu_array_info *array = &prog->array[i];
		isl_space *space;
		isl_set *read_i;
		int empty;

		if (gpu_array_is_read_only_scalar(array))
			continue;

		space = isl_space_copy(array->space);
		read_i = isl_union_set_extract_set(prog->copy_in, space);
		empty = isl_set_plain_is_empty(read_i);
		isl_set_free(read_i);

		p = allocate_device_array(p, array, !empty);
	}
	p = isl_printer_start_line(p);
	p = isl_printer_end_line(p);
	return p;
}

/* Print a call to the OpenCL clSetKernelArg() function which sets
 * the arguments of the kernel.  arg_name and arg_index are the name and the
 * index of the kernel argument.  The index of the leftmost argument of
 * the kernel is 0 whereas the index of the rightmost argument of the kernel
 * is n - 1, where n is the total number of the kernel arguments.
 * read_only_scalar is a boolean that indicates whether the argument is a read
 * only scalar.
 */
static __isl_give isl_printer *opencl_set_kernel_argument(
	__isl_take isl_printer *p, int kernel_id,
	const char *arg_name, int arg_index, int read_only_scalar)
{
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p,
		"openclCheckReturn(clSetKernelArg(kernel");
	p = isl_printer_print_int(p, kernel_id);
	p = isl_printer_print_str(p, ", ");
	p = isl_printer_print_int(p, arg_index);
	p = isl_printer_print_str(p, ", sizeof(");

	if (read_only_scalar) {
		p = isl_printer_print_str(p, arg_name);
		p = isl_printer_print_str(p, "), &");
	} else
		p = isl_printer_print_str(p, "cl_mem), (void *) &dev_");

	p = isl_printer_print_str(p, arg_name);
	p = isl_printer_print_str(p, "));");
	p = isl_printer_end_line(p);

	return p;
}

/* Print the block sizes as a list of the sizes in each
 * dimension.
 */
static __isl_give isl_printer *opencl_print_block_sizes(
	__isl_take isl_printer *p, struct ppcg_kernel *kernel)
{
	int i;

	if (kernel->n_block > 0)
		for (i = 0; i < kernel->n_block; ++i) {
			if (i)
				p = isl_printer_print_str(p, ", ");
			p = isl_printer_print_int(p, kernel->block_dim[i]);
		}
	else
		p = isl_printer_print_str(p, "1");

	return p;
}

/* Set the arguments of the OpenCL kernel by printing a call to the OpenCL
 * clSetKernelArg() function for each kernel argument.
 */
static __isl_give isl_printer *opencl_set_kernel_arguments(
	__isl_take isl_printer *p, struct gpu_prog *prog,
	struct ppcg_kernel *kernel)
{
	int i, n, ro;
	unsigned nparam;
	isl_space *space;
	const char *type;
	int arg_index = 0;

	for (i = 0; i < prog->n_array; ++i) {
		isl_set *arr;
		int empty;

		space = isl_space_copy(prog->array[i].space);
		arr = isl_union_set_extract_set(kernel->arrays, space);
		empty = isl_set_plain_is_empty(arr);
		isl_set_free(arr);
		if (empty)
			continue;
		ro = gpu_array_is_read_only_scalar(&prog->array[i]);
		opencl_set_kernel_argument(p, kernel->id, prog->array[i].name,
			arg_index, ro);
		arg_index++;
	}

	space = isl_union_set_get_space(kernel->arrays);
	nparam = isl_space_dim(space, isl_dim_param);
	for (i = 0; i < nparam; ++i) {
		const char *name;

		name = isl_space_get_dim_name(space, isl_dim_param, i);
		opencl_set_kernel_argument(p, kernel->id, name, arg_index, 1);
		arg_index++;
	}
	isl_space_free(space);

	n = isl_space_dim(kernel->space, isl_dim_set);
	for (i = 0; i < n; ++i) {
		const char *name;
		isl_id *id;

		name = isl_space_get_dim_name(kernel->space, isl_dim_set, i);
		opencl_set_kernel_argument(p, kernel->id, name, arg_index, 1);
		arg_index++;
	}

	return p;
}

/* Print the arguments to a kernel declaration or call.  If "types" is set,
 * then print a declaration (including the types of the arguments).
 *
 * The arguments are printed in the following order
 * - the arrays accessed by the kernel
 * - the parameters
 * - the host loop iterators
 */
static __isl_give isl_printer *opencl_print_kernel_arguments(
	__isl_take isl_printer *p, struct gpu_prog *prog,
	struct ppcg_kernel *kernel, int types)
{
	int i, n;
	int first = 1;
	unsigned nparam;
	isl_space *space;
	const char *type;

	for (i = 0; i < prog->n_array; ++i) {
		isl_set *arr;
		int empty;

		space = isl_space_copy(prog->array[i].space);
		arr = isl_union_set_extract_set(kernel->arrays, space);
		empty = isl_set_plain_is_empty(arr);
		isl_set_free(arr);
		if (empty)
			continue;

		if (!first)
			p = isl_printer_print_str(p, ", ");

		if (types)
			p = gpu_array_info_print_declaration_argument(p,
				&prog->array[i], "__global");
		else
			p = gpu_array_info_print_call_argument(p,
				&prog->array[i]);

		first = 0;
	}

	space = isl_union_set_get_space(kernel->arrays);
	nparam = isl_space_dim(space, isl_dim_param);
	for (i = 0; i < nparam; ++i) {
		const char *name;

		name = isl_space_get_dim_name(space, isl_dim_param, i);

		if (!first)
			p = isl_printer_print_str(p, ", ");
		if (types)
			p = isl_printer_print_str(p, "int ");
		p = isl_printer_print_str(p, name);

		first = 0;
	}
	isl_space_free(space);

	n = isl_space_dim(kernel->space, isl_dim_set);
	type = isl_options_get_ast_iterator_type(prog->ctx);
	for (i = 0; i < n; ++i) {
		const char *name;
		isl_id *id;

		if (!first)
			p = isl_printer_print_str(p, ", ");
		name = isl_space_get_dim_name(kernel->space, isl_dim_set, i);
		if (types) {
			p = isl_printer_print_str(p, type);
			p = isl_printer_print_str(p, " ");
		}
		p = isl_printer_print_str(p, name);

		first = 0;
	}

	return p;
}

/* Print the header of the given kernel.
 */
static __isl_give isl_printer *opencl_print_kernel_header(
	__isl_take isl_printer *p, struct gpu_prog *prog,
	struct ppcg_kernel *kernel)
{
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "__kernel void kernel");
	p = isl_printer_print_int(p, kernel->id);
	p = isl_printer_print_str(p, "(");
	p = opencl_print_kernel_arguments(p, prog, kernel, 1);
	p = isl_printer_print_str(p, ")");
	p = isl_printer_end_line(p);

	return p;
}

/* Unlike the equivalent function in the CUDA backend which prints iterators
 * in reverse order to promote coalescing, this function does not print
 * iterators in reverse order.  The OpenCL backend currently does not take
 * into account any coalescing considerations.
 */
static __isl_give isl_printer *opencl_print_kernel_iterators(
	__isl_take isl_printer *p, struct ppcg_kernel *kernel)
{
	int i, n_grid;
	isl_ctx *ctx = isl_ast_node_get_ctx(kernel->tree);
	const char *type;

	type = isl_options_get_ast_iterator_type(ctx);

	n_grid = isl_multi_pw_aff_dim(kernel->grid_size, isl_dim_set);
	if (n_grid > 0) {
		p = isl_printer_start_line(p);
		p = isl_printer_print_str(p, type);
		p = isl_printer_print_str(p, " ");
		for (i = 0; i < n_grid; ++i) {
			if (i)
				p = isl_printer_print_str(p, ", ");
			p = isl_printer_print_str(p, "b");
			p = isl_printer_print_int(p, i);
			p = isl_printer_print_str(p, " = get_group_id(");
			p = isl_printer_print_int(p, i);
			p = isl_printer_print_str(p, ")");
		}
		p = isl_printer_print_str(p, ";");
		p = isl_printer_end_line(p);
	}

	if (kernel->n_block > 0) {
		p = isl_printer_start_line(p);
		p = isl_printer_print_str(p, type);
		p = isl_printer_print_str(p, " ");
		for (i = 0; i < kernel->n_block; ++i) {
			if (i)
				p = isl_printer_print_str(p, ", ");
			p = isl_printer_print_str(p, "t");
			p = isl_printer_print_int(p, i);
			p = isl_printer_print_str(p, " = get_local_id(");
			p = isl_printer_print_int(p, i);
			p = isl_printer_print_str(p, ")");
		}
		p = isl_printer_print_str(p, ";");
		p = isl_printer_end_line(p);
	}

	return p;
}

static __isl_give isl_printer *opencl_print_kernel_var(
	__isl_take isl_printer *p, struct ppcg_kernel_var *var)
{
	int j;
	isl_val *v;

	p = isl_printer_start_line(p);
	if (var->type == ppcg_access_shared)
		p = isl_printer_print_str(p, "__local ");
	p = isl_printer_print_str(p, var->array->type);
	p = isl_printer_print_str(p, " ");
	p = isl_printer_print_str(p, var->name);
	for (j = 0; j < var->array->n_index; ++j) {
		p = isl_printer_print_str(p, "[");
		v = isl_vec_get_element_val(var->size, j);
		p = isl_printer_print_val(p, v);
		p = isl_printer_print_str(p, "]");
		isl_val_free(v);
	}
	p = isl_printer_print_str(p, ";");
	p = isl_printer_end_line(p);

	return p;
}

static __isl_give isl_printer *opencl_print_kernel_vars(
		__isl_take isl_printer *p, struct ppcg_kernel *kernel)
{
	int i;

	for (i = 0; i < kernel->n_var; ++i)
		p = opencl_print_kernel_var(p, &kernel->var[i]);

	return p;
}

/* Print a call to barrier() which is a sync statement.
 * All work-items in a work-group executing the kernel on a processor must
 * execute the barrier() function before any are allowed to continue execution
 * beyond the barrier.
 * The flag CLK_LOCAL_MEM_FENCE makes the barrier function either flush any
 * variables stored in local memory or queue a memory fence to ensure correct
 * ordering of memory operations to local memory.
 * The flag CLK_GLOBAL_MEM_FENCE makes the barrier function queue a memory
 * fence to ensure correct ordering of memory operations to global memory.
 */
static __isl_give isl_printer *opencl_print_sync(__isl_take isl_printer *p,
	struct ppcg_kernel_stmt *stmt)
{
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p,
		"barrier(CLK_LOCAL_MEM_FENCE | CLK_GLOBAL_MEM_FENCE);");
	p = isl_printer_end_line(p);

	return p;
}

/* This function is called for each user statement in the AST,
 * i.e., for each kernel body statement, copy statement or sync statement.
 */
static __isl_give isl_printer *opencl_print_kernel_stmt(
	__isl_take isl_printer *p,
	__isl_take isl_ast_print_options *print_options,
	__isl_keep isl_ast_node *node, void *user)
{
	isl_id *id;
	struct ppcg_kernel_stmt *stmt;

	id = isl_ast_node_get_annotation(node);
	stmt = isl_id_get_user(id);
	isl_id_free(id);

	isl_ast_print_options_free(print_options);

	switch (stmt->type) {
	case ppcg_kernel_copy:
		return ppcg_kernel_print_copy(p, stmt);
	case ppcg_kernel_sync:
		return opencl_print_sync(p, stmt);
	case ppcg_kernel_domain:
		return ppcg_kernel_print_domain(p, stmt);
	}

	return p;
}

/* Return true if there is a double array in prog->array or
 * if any of the types in prog->scop involve any doubles.
 * To check the latter condition, we simply search for the string "double"
 * in the type definitions, which may result in false positives.
 */
static __isl_give int any_double_elements(struct gpu_prog *prog)
{
	int i;

	for (i = 0; i < prog->n_array; ++i)
		if (strcmp(prog->array[i].type, "double") == 0)
			return 1;

	for (i = 0; i < prog->scop->n_type; ++i) {
		struct pet_type *type = prog->scop->types[i];

		if (strstr(type->definition, "double"))
			return 1;
	}

	return 0;
}

/* Prints a #pragma to enable support for double floating-point
 * precision.  OpenCL 1.0 adds support for double precision floating-point as
 * an optional extension. An application that wants to use double will need to
 * include the #pragma OPENCL EXTENSION cl_khr_fp64 : enable directive before
 * any double precision data type is declared in the kernel code.
 */
static __isl_give isl_printer *opencl_enable_double_support(
	__isl_take isl_printer *p)
{
	int i;

	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "#pragma OPENCL EXTENSION cl_khr_fp64 :"
		" enable");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_end_line(p);

	return p;
}

static void opencl_print_kernel(struct gpu_prog *prog,
	struct ppcg_kernel *kernel, struct opencl_info *opencl)
{
	isl_ctx *ctx = isl_ast_node_get_ctx(kernel->tree);
	isl_ast_print_options *print_options;
	isl_printer *p;

	p = isl_printer_to_file(ctx, opencl->kernel_c);
	print_options = isl_ast_print_options_alloc(ctx);
	print_options = isl_ast_print_options_set_print_user(print_options,
				&opencl_print_kernel_stmt, NULL);

	p = isl_printer_set_output_format(p, ISL_FORMAT_C);
	p = opencl_print_kernel_header(p, prog, kernel);
	p = isl_printer_print_str(p, "{");
	p = isl_printer_end_line(p);
	p = isl_printer_indent(p, 4);
	p = opencl_print_kernel_iterators(p, kernel);
	p = opencl_print_kernel_vars(p, kernel);
	p = isl_printer_end_line(p);
	p = gpu_print_macros(p, kernel->tree);
	p = isl_ast_node_print(kernel->tree, p, print_options);
	p = isl_printer_print_str(p, "}");
	p = isl_printer_end_line(p);
	isl_printer_free(p);
}

struct print_host_user_data_opencl {
	struct opencl_info *opencl;
	struct gpu_prog *prog;
};

/* This function prints the i'th block size multiplied by the i'th grid size,
 * where i (a parameter to this function) is one of the possible dimensions of
 * grid sizes and block sizes.
 * If the dimension of block sizes is not equal to the dimension of grid sizes
 * the output is calculated as follows:
 *
 * Suppose that:
 * block_sizes[dim1] is the list of blocks sizes and it contains dim1 elements.
 * grid_sizes[dim2] is the list of grid sizes and it contains dim2 elements.
 *
 * The output is:
 * If (i > dim2) then the output is block_sizes[i]
 * If (i > dim1) then the output is grid_sizes[i]
 */
static __isl_give isl_printer *opencl_print_total_number_of_work_items_for_dim(
	__isl_take isl_printer *p, struct ppcg_kernel *kernel, int i)
{
	int grid_dim, block_dim;

	grid_dim = isl_multi_pw_aff_dim(kernel->grid_size, isl_dim_set);
	block_dim = kernel->n_block;

	isl_pw_aff *bound_grid;

	if (i < min(grid_dim, block_dim)) {
		bound_grid = isl_multi_pw_aff_get_pw_aff(kernel->grid_size, i);
		p = isl_printer_print_str(p, "(");
		p = isl_printer_print_pw_aff(p, bound_grid);
		p = isl_printer_print_str(p, ") * ");
		p = isl_printer_print_int(p, kernel->block_dim[i]);
		isl_pw_aff_free(bound_grid);
	} else if (i >= grid_dim)
		p = isl_printer_print_int(p, kernel->block_dim[i]);
	else {
		bound_grid = isl_multi_pw_aff_get_pw_aff(kernel->grid_size, i);
		p = isl_printer_print_pw_aff(p, bound_grid);
		isl_pw_aff_free(bound_grid);
	}

	return p;
}

/* Print a list that represents the total number of work items.  The list is
 * constructed by performing an element-wise multiplication of the block sizes
 * and the grid sizes.  To explain how the list is constructed, suppose that:
 * block_sizes[dim1] is the list of blocks sizes and it contains dim1 elements.
 * grid_sizes[dim2] is the list of grid sizes and it contains dim2 elements.
 *
 * The output of this function is constructed as follows:
 * If (dim1 > dim2) then the output is the following list:
 * grid_sizes[0]*block_sizes[0], ..., grid_sizes[dim2-1]*block_sizes[dim2-1],
 * block_sizes[dim2], ..., block_sizes[dim1-2], block_sizes[dim1-1].
 *
 * If (dim2 > dim1) then the output is the following list:
 * grid_sizes[0]*block_sizes[0], ..., grid_sizes[dim1-1] * block_sizes[dim1-1],
 * grid_sizes[dim1], grid_sizes[dim2-2], grid_sizes[dim2-1].
 *
 * To calculate the total number of work items out of the list constructed by
 * this function, the user should multiply the elements of the list.
 */
static __isl_give isl_printer *opencl_print_total_number_of_work_items_as_list(
	__isl_take isl_printer *p, struct ppcg_kernel *kernel)
{
	int i;
	int grid_dim, block_dim;

	grid_dim = isl_multi_pw_aff_dim(kernel->grid_size, isl_dim_set);
	block_dim = kernel->n_block;

	if ((grid_dim <= 0) || (block_dim <= 0)) {
		p = isl_printer_print_str(p, "1");
		return p;
	}

	for (i = 0; i <= max(grid_dim, block_dim) - 1; i++) {
		if (i > 0)
			p = isl_printer_print_str(p, ", ");

		p = opencl_print_total_number_of_work_items_for_dim(p,
			kernel, i);
	}

	return p;
}

/* Print the user statement of the host code to "p".
 *
 * In particular, print a block of statements that defines the grid
 * and the work group and then launches the kernel.
 *
 * A grid is composed of many work groups (blocks), each work group holds
 * many work-items (threads).
 *
 * global_work_size[kernel->n_block] represents the total number of work
 * items.  It points to an array of kernel->n_block unsigned
 * values that describe the total number of work-items that will execute
 * the kernel.  The total number of work-items is computed as:
 * global_work_size[0] *...* global_work_size[kernel->n_block - 1].
 *
 * The size of each work group (i.e. the number of work-items in each work
 * group) is described using block_size[kernel->n_block].  The total
 * number of work-items in a block (work-group) is computed as:
 * block_size[0] *... * block_size[kernel->n_block - 1].
 *
 * For more information check:
 * http://www.khronos.org/registry/cl/sdk/1.0/docs/man/xhtml/clEnqueueNDRangeKernel.html
 */
static __isl_give isl_printer *opencl_print_host_user(
	__isl_take isl_printer *p,
	__isl_take isl_ast_print_options *print_options,
	__isl_keep isl_ast_node *node, void *user)
{
	isl_id *id;
	struct ppcg_kernel *kernel;
	struct print_host_user_data_opencl *data;
	int i;

	id = isl_ast_node_get_annotation(node);
	kernel = isl_id_get_user(id);
	isl_id_free(id);

	data = (struct print_host_user_data_opencl *) user;

	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "{");
	p = isl_printer_end_line(p);
	p = isl_printer_indent(p, 2);

	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "size_t global_work_size[");

	if (kernel->n_block > 0)
		p = isl_printer_print_int(p, kernel->n_block);
	else
		p = isl_printer_print_int(p, 1);

	p = isl_printer_print_str(p, "] = {");
	p = opencl_print_total_number_of_work_items_as_list(p, kernel);
	p = isl_printer_print_str(p, "};");
	p = isl_printer_end_line(p);

	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "size_t block_size[");

	if (kernel->n_block > 0)
		p = isl_printer_print_int(p, kernel->n_block);
	else
		p = isl_printer_print_int(p, 1);

	p = isl_printer_print_str(p, "] = {");
	p = opencl_print_block_sizes(p, kernel);
	p = isl_printer_print_str(p, "};");
	p = isl_printer_end_line(p);

	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "cl_kernel kernel");
	p = isl_printer_print_int(p, kernel->id);
	p = isl_printer_print_str(p, " = clCreateKernel(program, \"kernel");
	p = isl_printer_print_int(p, kernel->id);
	p = isl_printer_print_str(p, "\", &err);");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "openclCheckReturn(err);");
	p = isl_printer_end_line(p);

	opencl_set_kernel_arguments(p, data->prog, kernel);

	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "openclCheckReturn(clEnqueueNDRangeKernel"
		"(queue, kernel");
	p = isl_printer_print_int(p, kernel->id);
	p = isl_printer_print_str(p, ", ");
	if (kernel->n_block > 0)
		p = isl_printer_print_int(p, kernel->n_block);
	else
		p = isl_printer_print_int(p, 1);

	p = isl_printer_print_str(p, ", NULL, global_work_size, "
					"block_size, "
					"0, NULL, NULL));");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "openclCheckReturn("
					"clReleaseKernel(kernel");
	p = isl_printer_print_int(p, kernel->id);
	p = isl_printer_print_str(p, "));");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "clFinish(queue);");
	p = isl_printer_end_line(p);
	p = isl_printer_indent(p, -2);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "}");
	p = isl_printer_end_line(p);

	p = isl_printer_start_line(p);
	p = isl_printer_end_line(p);

	opencl_print_kernel(data->prog, kernel, data->opencl);

	isl_ast_print_options_free(print_options);

	return p;
}

static __isl_give isl_printer *opencl_print_host_code(
	__isl_take isl_printer *p, struct gpu_prog *prog,
	__isl_keep isl_ast_node *tree, struct opencl_info *opencl)
{
	isl_ast_print_options *print_options;
	isl_ctx *ctx = isl_ast_node_get_ctx(tree);
	struct print_host_user_data_opencl data = { opencl, prog };

	print_options = isl_ast_print_options_alloc(ctx);
	print_options = isl_ast_print_options_set_print_user(print_options,
				&opencl_print_host_user, &data);

	p = gpu_print_macros(p, tree);
	p = isl_ast_node_print(tree, p, print_options);

	return p;
}

/* Copy "array" back from the GPU to the host.
 */
static __isl_give isl_printer *copy_array_from_device(__isl_take isl_printer *p,
	void *user)
{
	struct gpu_array_info *array = user;

	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "openclCheckReturn("
					"clEnqueueReadBuffer(queue,"
					" dev_");
	p = isl_printer_print_str(p, array->name);
	p = isl_printer_print_str(p, ", CL_TRUE, 0, ");
	p = gpu_array_info_print_size(p, array);

	if (gpu_array_is_scalar(array))
		p = isl_printer_print_str(p, ", &");
	else
		p = isl_printer_print_str(p, ", ");
	p = isl_printer_print_str(p, array->name);
	p = isl_printer_print_str(p, ", 0, NULL, NULL));");
	p = isl_printer_end_line(p);

	return p;
}

/* Copy copy_out arrays back from the GPU to the host.
 *
 * Only perform the copying for arrays with strictly positive size.
 */
static __isl_give isl_printer *opencl_copy_arrays_from_device(
	__isl_take isl_printer *p, struct gpu_prog *prog)
{
	int i;
	isl_union_set *copy_out;
	copy_out = isl_union_set_copy(prog->copy_out);

	for (i = 0; i < prog->n_array; ++i) {
		struct gpu_array_info *array = &prog->array[i];
		isl_space *space;
		isl_set *copy_out_i;
		isl_set *guard;
		int empty;

		space = isl_space_copy(array->space);
		copy_out_i = isl_union_set_extract_set(copy_out, space);
		empty = isl_set_plain_is_empty(copy_out_i);
		isl_set_free(copy_out_i);
		if (empty)
			continue;

		guard = gpu_array_positive_size_guard(array);
		p = ppcg_print_guarded(p, guard, isl_set_copy(prog->context),
					    &copy_array_from_device, array);
	}

	isl_union_set_free(copy_out);
	p = isl_printer_start_line(p);
	p = isl_printer_end_line(p);
	return p;
}

/* Create an OpenCL device, context, command queue and build the kernel.
 * input is the name of the input file provided to ppcg.
 */
static __isl_give isl_printer *opencl_setup(__isl_take isl_printer *p,
	const char *input, struct opencl_info *info)
{
	int len;

	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "cl_device_id device;");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "cl_context context;");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "cl_program program;");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "cl_command_queue queue;");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "cl_int err;");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "device = opencl_create_device(");
	p = isl_printer_print_int(p, info->options->opencl_use_gpu);
	p = isl_printer_print_str(p, ");");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "context = clCreateContext(NULL, 1, "
		"&device, NULL, NULL, &err);");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "openclCheckReturn(err);");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "queue = clCreateCommandQueue"
					"(context, device, 0, &err);");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "openclCheckReturn(err);");
	p = isl_printer_end_line(p);

	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "program = opencl_build_program("
					"context, device, \"");
	p = isl_printer_print_str(p, info->kernel_c_name);
	p = isl_printer_print_str(p, "\", \"");

	if (info->options->opencl_compiler_options)
		p = isl_printer_print_str(p,
					info->options->opencl_compiler_options);

	p = isl_printer_print_str(p, "\");");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_end_line(p);

	return p;
}

static __isl_give isl_printer *opencl_release_cl_objects(
	__isl_take isl_printer *p, struct opencl_info *info)
{
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "openclCheckReturn(clReleaseCommandQueue"
					"(queue));");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "openclCheckReturn(clReleaseProgram"
					"(program));");
	p = isl_printer_end_line(p);
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "openclCheckReturn(clReleaseContext"
					"(context));");
	p = isl_printer_end_line(p);

	return p;
}

/* Free the device array corresponding to "array"
 */
static __isl_give isl_printer *release_device_array(__isl_take isl_printer *p,
	struct gpu_array_info *array)
{
	p = isl_printer_start_line(p);
	p = isl_printer_print_str(p, "openclCheckReturn("
					"clReleaseMemObject(dev_");
	p = isl_printer_print_str(p, array->name);
	p = isl_printer_print_str(p, "));");
	p = isl_printer_end_line(p);

	return p;
}

/* Free the device arrays.
 */
static __isl_give isl_printer *opencl_release_device_arrays(
	__isl_take isl_printer *p, struct gpu_prog *prog)
{
	int i, j;

	for (i = 0; i < prog->n_array; ++i) {
		struct gpu_array_info *array = &prog->array[i];
		if (gpu_array_is_read_only_scalar(array))
			continue;

		p = release_device_array(p, array);
	}
	return p;
}

/* Given a gpu_prog "prog" and the corresponding transformed AST
 * "tree", print the entire OpenCL code to "p".
 */
static __isl_give isl_printer *print_opencl(__isl_take isl_printer *p,
	struct gpu_prog *prog, __isl_keep isl_ast_node *tree,
	struct gpu_types *types, void *user)
{
	struct opencl_info *opencl = user;
	isl_printer *kernel;

	kernel = isl_printer_to_file(isl_printer_get_ctx(p), opencl->kernel_c);
	kernel = isl_printer_set_output_format(kernel, ISL_FORMAT_C);
	if (any_double_elements(prog))
		kernel = opencl_enable_double_support(kernel);
	kernel = gpu_print_types(kernel, types, prog);
	isl_printer_free(kernel);

	if (!kernel)
		return isl_printer_free(p);

	p = ppcg_start_block(p);

	p = opencl_print_host_macros(p);

	p = opencl_declare_device_arrays(p, prog);
	p = opencl_setup(p, opencl->input, opencl);
	p = opencl_allocate_device_arrays(p, prog);

	p = opencl_print_host_code(p, prog, tree, opencl);

	p = opencl_copy_arrays_from_device(p, prog);
	p = opencl_release_device_arrays(p, prog);
	p = opencl_release_cl_objects(p, opencl);

	p = ppcg_end_block(p);

	return p;
}

/* Transform the code in the file called "input" by replacing
 * all scops by corresponding OpenCL code.
 * The host code is written to "output" or a name derived from
 * "input" if "output" is NULL.
 * The kernel code is placed in separate files with names
 * derived from "output" or "input".
 *
 * We let generate_gpu do all the hard work and then let it call
 * us back for printing the AST in print_cuda.
 *
 * To prepare for this printing, we first open the output files
 * and we close them after generate_gpu has finished.
 */
int generate_opencl(isl_ctx *ctx, struct ppcg_options *options,
	const char *input, const char *output)
{
	struct opencl_info opencl = { options, input, output };
	int r;

	r = opencl_open_files(&opencl);

	if (r >= 0)
		r = generate_gpu(ctx, input, opencl.host_c, options,
				&print_opencl, &opencl);

	opencl_close_files(&opencl);

	return r;
}