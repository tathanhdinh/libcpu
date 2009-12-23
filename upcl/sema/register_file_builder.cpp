#include "sema/register_file_builder.h"
#include "sema/register_info.h"
#include "sema/convert.h"

#include "c/sub_register_def.h"
#include "c/bound_sub_register_def.h"
#include "c/hardwired_sub_register_def.h"

#include <sstream>
extern "C" {
#include "strnatcmp.h"
}

using namespace upcl;
using upcl::sema::register_file_builder;
using upcl::sema::register_dep_tracker;
using upcl::sema::register_info;

static std::string
make_c_compat_name(std::string const &name)
{
	size_t pos;
	pos = name.find('$');
	if (pos != std::string::npos) {
		std::string unnamed("__unnamed" + name);

		while ((pos = unnamed.find('$')) != std::string::npos)
			unnamed[pos] = '_';

		return unnamed;
	}
	return name;
}

static void
cg_print_typed_var(size_t data_size, size_t nbits,
		std::string const &name)
{
	printf("uint%zu_t %s", data_size,
			make_c_compat_name(name).c_str());

	if (nbits > 64)
		printf("[%zu]", nbits / data_size);
	else if (nbits < data_size)
		printf(" : %zu", nbits);

	printf (";\n");
}

static void
cg_dump(c::register_def *def, bool maybe_unused = true)
{
	size_t nbits;
	if (def->is_sub_register())
		nbits = ((c::sub_register_def *)def)->get_master_register()->
		 	get_type()->get_bits();
	else
		nbits = def->get_type()->get_bits();

	if (nbits <= 8)
		nbits = 8;
	else if (nbits <= 16)
		nbits = 16;
	else if (nbits <= 32)
		nbits = 32;
	else if (nbits <= 64)
		nbits = 64;
	else
		nbits = ((nbits + 63) / 64) * 64;

	c::sub_register_vector const &sub =
		def->get_sub_register_vector();

	if (!sub.empty()) {
		printf("union {\n");
		cg_print_typed_var(nbits, def->get_type()->get_bits(),
				def->get_name());
		printf("struct {\n");
		for(c::sub_register_vector::const_iterator i = sub.begin();
				i != sub.end(); i++)
			cg_dump(*i, false);
		printf("};\n");
		printf("} %s;\n", def->get_name().c_str());
	} else {
		cg_print_typed_var(nbits, def->get_type()->get_bits(),
				def->get_name());
		if (maybe_unused && nbits <= 64 &&
				nbits > def->get_type()->get_bits()) {
			cg_print_typed_var(nbits, nbits - def->get_type()->get_bits(),
				"__unused_" + def->get_name());
		}
	}
}


namespace {

// represent a group of registers with same
// name and type.
struct regset {
	std::string          name;
	std::string          type;
	sema::register_info_vector regs;
};

typedef std::vector <regset> regset_vector;

struct register_info_nat_sort {
	inline bool operator()(register_info const *a,
			register_info const *b) const
	{ return (::strnatcmp(a->name.c_str(), b->name.c_str()) < 0); }
};

static std::string
drop_digits(std::string const &x)
{
	size_t n = x.length();

	if (n == 0)
		return std::string();

	while (isdigit(x[--n]))
		;

	return x.substr(0, n + 1);
}

static std::string
inc_name(std::string const &name)
{
	long index = 1;
	std::string base_name(drop_digits(name));
	if (base_name.length() < name.length()) {
		index = atol(name.substr(base_name.length()).c_str());
		index++;
	}
	std::stringstream ss;
	ss << name << index;
	return ss.str();
}

static inline bool is_pseudo_reg(register_info const *reg) {
	return (reg->name[0] == '%' || reg->name[0] == '$' ||
			reg->name[reg->name.length() - 1] == '?');
}

}

register_file_builder::register_file_builder()
{
}

static void
make_regsets(sema::register_info_vector const &regs,
		regset_vector &regsets)
{
	string_set regset_id;

	std::string last;
	std::string last_type;
	sema::register_info_vector regs_in_set;

	for (sema::register_info_vector::const_iterator i = regs.begin();
			i != regs.end(); i++) {

		// ignore pseudo registers
		if (is_pseudo_reg(*i))
			continue;

		std::string x = drop_digits((*i)->name);
		if (last.empty() || x != last ||
				(*i)->type->get_value() != last_type) {

			if (!last.empty()) {
				regset rs;

				while (regset_id.find(last) != regset_id.end())
					last = inc_name(last);

				rs.name = last;
				rs.type = last_type;
				rs.regs = regs_in_set;

				regsets.push_back(rs);
				regset_id.insert(rs.name);
			}

			last = x;
			if ((*i)->type != 0)
				last_type = (*i)->type->get_value();
			else
				last_type.clear();

			regs_in_set.clear();
			regs_in_set.push_back(*i);
		} else {
			regs_in_set.push_back(*i);
		}
	}

	if (!regs_in_set.empty()) {
		regset rs;

		while (regset_id.find(last) != regset_id.end())
			last = inc_name(last);

		rs.name = last;
		rs.type = last_type;
		rs.regs = regs_in_set;

		regsets.push_back(rs);
		regset_id.insert(rs.name);
	}
}

bool
register_file_builder::analyze(register_dep_tracker *rdt)
{
	printf("register_file_builder starts analysis.\n");

	rdt->resolve_subs();
	//rdt->dump();

	// find all registers with no dependencies.
	
	register_info_vector regs;

	rdt->get_indep_regs(regs);

	// sort registers naturally.
	std::sort(regs.begin(), regs.end(), register_info_nat_sort());

	// group register sets.
	regset_vector regsets;

	make_regsets(regs, regsets);

	// analyze top independent registers
	for(regset_vector::const_iterator i = regsets.begin();
			i != regsets.end(); i++) {

		if (i->regs.size() == 1) {
			if (!analyze_top(i->regs[0]))
				return false;
		} else {
			printf("Register Set '%s' Type '%s' Count %zu\n", 
					i->name.c_str(), i->type.c_str(),
					i->regs.size());
		}
	}

	return true;
}

std::vector<c::register_def *> g_rdefs;

bool
register_file_builder::analyze_top(register_info const *ri)
{
	// XXX CHECK BINDING TO SPECIAL REGISTER!
	printf("Register '%s' Type '%s'\n", 
			ri->name.c_str(), ri->type->get_value().c_str());

	c::type *rtype = convert_type(ri->type);
	if (rtype == 0)
		return false;

	c::register_def *rdef = new c::register_def(ri->name, rtype);
	if (rdef == 0) {
		fprintf(stderr, "error: cannot create register.\n");
		return false;
	}

	for(register_info_vector::const_iterator i = ri->subs.begin();
			i != ri->subs.end(); i++) {

		c::register_def *sub = create_sub(ri, rdef, *i);
		if (sub == 0)
			return false;
	}

	cg_dump(rdef);

	g_rdefs.push_back(rdef);
	return true;
}

c::register_def *
register_file_builder::create_sub(register_info const *top_ri,
		c::register_def *top_rdef, register_info const *sub_ri)
{
	c::type *type = 0;
	c::register_def *rdef = 0;

//	printf("analyze_sub(%s)\n", sub_ri->name.c_str());

	type = convert_type(sub_ri->type);
	if (type == 0)
		return 0;

	// if this is register is an hardwired expression, translate
	// the expression.
	if (sub_ri->hwexpr != 0) {
		c::expression *expr = convert_expression(sub_ri->hwexpr);
		if (expr == 0)
			return 0;

		rdef = new c::hardwired_sub_register_def(top_rdef, sub_ri->name,
				type, sub_ri->bit_start, type->get_bits(),
				expr);
	}
	// if this register is bound bidirectionally to a register,
	// this register is virtual and it's an indirect reference
	// to the bound register.
	else if (sub_ri->flags & register_info::BIDIBIND_FLAG) {
		c::type *bind_type;
		register_info *binding_ri;

//		printf("BIDIBIND!\n");
		if (sub_ri->bind_copy != 0) {
			// XXX
			printf("BIDIBIND copy?\n");
			return 0;
		} else {
			binding_ri = sub_ri->binding;
		}

		bind_type = convert_type(binding_ri->type);
		if (bind_type == 0)
			return 0;

		if (!bind_type->is_equal(type)) {
			fprintf(stderr, "error: bidirectional binding '%s' requires that "
					"the bitfield '%s' size (%zu) matches the "
					"aliased register (%s) size (%zu) and type.\n",
					top_ri->name.c_str(),
					sub_ri->name.c_str(),
					type->get_bits(),
					binding_ri->name.c_str(),
					bind_type->get_bits());
			return 0;
		}

		rdef = create_aliased_sub(top_ri, top_rdef, type, sub_ri,
				binding_ri);

		// XXX add alternative name if this is a named bitfield
	}
	// if this register is bound to something check it.
	else if (sub_ri->binding != 0) {
		// special handling is required when binding
		// pseudo registers.
		if (sub_ri->binding->name[0] == '%') {
			rdef = create_pseudo_aliased_sub(top_ri, top_rdef, type,
					sub_ri);
		}
		// otherwise it is bound to a register as a update-on-write.
		else {
			rdef = new c::sub_register_def(top_rdef, sub_ri->name,
					type, sub_ri->bit_start, type->get_bits(),
					true);

			// if the bound register has been generated, reference it,
			// otherwise record for lazy resolving.
			//
			// find first in the sub registers of our top register.
			c::register_def *bound =
				top_rdef->get_sub_register(sub_ri->binding->name);
			
			if (bound == 0)
				bound = 0; // XXX resolve globally

			if (bound != 0) {
				if (!bound->add_uow(rdef)) {
					fprintf(stderr, "fatal error: register '%s' binds to itself.\n",
							bound->get_name().c_str());
					return 0;
				}
			} else {
				// record for lazy resolving.
				//m_lazy_uow[sub_ri->binding->name].push_back(rdef);
				assert(0 && "IMPLEMENT ME ! (record lazy resolving!)");
			}
		}
	}
	// if this register has a special evaluation function,
	// then subregisters total size may be different from
	// the final size, so that's treated specially.
	else if (sub_ri->special_eval != 0) {
	} else {
		// a simple sub register.
		rdef = new c::sub_register_def(top_rdef, sub_ri->name,
				type, sub_ri->bit_start, type->get_bits(),
				true);
	}

	if (rdef != 0) {
//		fprintf(stderr, "CREATED SUBREG %s\n",
//				rdef->get_name().c_str());

		for(register_info_vector::const_iterator i = sub_ri->subs.begin();
				i != sub_ri->subs.end(); i++) {

			c::register_def *sub = create_sub(sub_ri, rdef, *i);
			if (sub == 0)
				return 0;
		}
	} else {
		fprintf(stderr, "FAILED CREATING REG %s\n", 
				sub_ri->name.c_str());
	}

	return rdef;
}

c::register_def *
register_file_builder::create_aliased_sub(register_info const *top_ri,
		c::register_def *top_rdef, c::type const *type,
		register_info const *sub, register_info const *alias)
{
	c::register_def *rdef;

	if (alias == sub) {
		fprintf(stderr, "error: register can't alias itself.\n");
		return 0;
	}

	// first create the bound register.
	rdef = create_sub(top_ri, top_rdef, alias);
	if (rdef == 0)
		return 0;

	// then change its subfield range.
	((c::sub_register_def *)rdef)->set_aliasing_range(sub->bit_start,
		type->get_bits());

	return rdef;
}

c::register_def *
register_file_builder::create_pseudo_aliased_sub(register_info const *top_ri,
		c::register_def *top_rdef, c::type *type,
		register_info const *sub_ri)
{
	c::register_def *rdef = 0;

	// pseudo registers bindable at sub level are only
	// conditional flags:
	//
	// C, N, P, V, Z (1 bit)
	//
	std::string pseudo_name (sub_ri->binding->name.substr(1));

	if (pseudo_name != "C" && pseudo_name != "N" && pseudo_name != "P" &&
			pseudo_name != "V" && pseudo_name != "Z") {
		fprintf(stderr, "error: only conditional pseudo registers may be "
				"aliased in bitfields.\n");
		return 0;
	}

	// the size of the sub register shall always be 1.
	if (type->get_bits() != 1) {
		fprintf(stderr, "error: bound conditional bit flag is %zu bits in "
				"size, it shall be one.\n", type->get_bits());
		return 0;
	}

#if 0
	fprintf(stderr, "BIND PSR!\n");
	// CC flags shall always part of a register bound to the
	// pseudo register PSR.
	if (!top_rdef->is_psr()) {
		fprintf(stderr, "error: conditional flags may be aliased only when "
				"parent register is bound to the pseudo register %PSR.\n");
		return 0;
	}
#endif

	c::special_register preg;

	switch (pseudo_name[0]) {
		case 'C': preg = c::SPECIAL_REGISTER_C; break;
		case 'N': preg = c::SPECIAL_REGISTER_N; break;
		case 'P': preg = c::SPECIAL_REGISTER_P; break;
		case 'V': preg = c::SPECIAL_REGISTER_V; break;
		case 'Z': preg = c::SPECIAL_REGISTER_Z; break;
	}

	rdef = new c::bound_sub_register_def(top_rdef, sub_ri->name,
			type, sub_ri->bit_start, type->get_bits(), preg,
			true);

	return rdef;
}