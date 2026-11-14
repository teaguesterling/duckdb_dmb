#pragma once

#include "block_tree.hpp"
#include "selector.hpp"

#include <memory>
#include <unordered_map>

namespace duckdb_re2 {
class RE2;
}

namespace duckdb {
namespace dmb {

//! The rows of a heading's section: the heading and every following row until a heading of the
//! same or higher rank at its level, or a row at a smaller level. Inline rows included.
vector<idx_t> SectionExtent(const BlockTree &tree, idx_t heading);

//! Matches selectors against one document. Holds per-document caches, so make one per tree.
class Matcher {
public:
	explicit Matcher(const BlockTree &tree);
	~Matcher();
	//! Throws NotImplementedException for a pseudo-class dmb accepts but can't match yet.
	bool Matches(const ComplexSelector &sel, idx_t node);

private:
	bool MatchFrom(const ComplexSelector &sel, idx_t k, idx_t node, int64_t anchor);
	bool MatchCompound(const Compound &c, idx_t node);
	bool MatchSimple(const SimpleSelector &s, idx_t node);
	bool Related(char comb, idx_t node, idx_t anchor) const;
	bool InSection(const SimpleSelector &s, idx_t node);
	duckdb_re2::RE2 &Regex(const string &pattern);

	const BlockTree &tree;
	std::unordered_map<const SimpleSelector *, vector<bool>> sections;
	std::unordered_map<string, std::unique_ptr<duckdb_re2::RE2>> regexes;
};

} // namespace dmb
} // namespace duckdb
