#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace dmb {

//! CSS specificity (a, b, c), compared lexicographically.
struct Specificity {
	int32_t a = 0, b = 0, c = 0;
	bool operator<(const Specificity &o) const {
		if (a != o.a) {
			return a < o.a;
		}
		if (b != o.b) {
			return b < o.b;
		}
		return c < o.c;
	}
	Specificity &operator+=(const Specificity &o) {
		a += o.a;
		b += o.b;
		c += o.c;
		return *this;
	}
};

struct ComplexSelector;

struct SimpleSelector {
	enum Kind { TYPE, UNIVERSAL, ID, CLASS, ATTR, PSEUDO_CLASS };
	Kind kind = UNIVERSAL;
	string name;
	string op;    // ATTR: "", "=", "~=", "^=", "$=", "*=", "|="
	string value; // ATTR: the value, quotes stripped
	vector<ComplexSelector> args; // :not(), :has(), :section()
	string text_arg;              // :contains(), :matches(), :h(), :callout(), :lang()
	int32_t a = 0, b = 0;         // :nth-child(an+b), :nth-of-type(an+b)
};

struct Compound {
	vector<SimpleSelector> parts; // empty = any block
};

struct ComplexSelector {
	vector<Compound> compounds;
	vector<char> combinators; // ' ', '>', '+', '~'; size = compounds.size() - 1
	char leading = 0;         // a relative selector, as in :has(> img)
	Specificity specificity;
};

//! The specificity of a compiled selector, without any pseudo-element.
Specificity ComputeSpecificity(const ComplexSelector &sel);

} // namespace dmb
} // namespace duckdb
