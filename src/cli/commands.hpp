// SPDX-License-Identifier: MIT
//
// One function per command. Each takes parsed arguments and returns an exit
// code; none of them contain engine logic, which is what keeps the CLI thin
// enough that everything it can do is also testable from C++.
#pragma once

#include <ostream>

#include "cli/args.hpp"

namespace vectordb::cli {

/// Options shared by every command.
struct GlobalOptions {
    bool json = false;
    bool quiet = false;
};

int command_create(const Args& args, const GlobalOptions& global, std::ostream& out);
int command_info(const Args& args, const GlobalOptions& global, std::ostream& out);
int command_stats(const Args& args, const GlobalOptions& global, std::ostream& out);
int command_insert(const Args& args, const GlobalOptions& global, std::ostream& out);
int command_get(const Args& args, const GlobalOptions& global, std::ostream& out);
int command_delete(const Args& args, const GlobalOptions& global, std::ostream& out);
int command_search(const Args& args, const GlobalOptions& global, std::ostream& out);
int command_batch_search(const Args& args, const GlobalOptions& global, std::ostream& out);
int command_import(const Args& args, const GlobalOptions& global, std::ostream& out);
int command_export(const Args& args, const GlobalOptions& global, std::ostream& out);
int command_generate(const Args& args, const GlobalOptions& global, std::ostream& out);
int command_rebuild_index(const Args& args, const GlobalOptions& global, std::ostream& out);
int command_compact(const Args& args, const GlobalOptions& global, std::ostream& out);
int command_check(const Args& args, const GlobalOptions& global, std::ostream& out);

}  // namespace vectordb::cli
