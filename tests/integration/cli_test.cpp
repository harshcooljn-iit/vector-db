// SPDX-License-Identifier: MIT
//
// Drives the real `vectordb` executable.
//
// These tests deliberately assert on *exit codes* and on stable substrings,
// never on exact layout. A test that breaks when a column widens is a test
// people delete. Machine-readable assertions go through `--json`, which is the
// contract that is actually promised.
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/wait.h>

#include <gtest/gtest.h>

#include "support/temp_dir.hpp"

namespace vectordb {
namespace {

using testing::TempDir;

// Named CliRun rather than Run: ::testing::Test has a private member of that
// name, and an unqualified `Run` inside a TEST body resolves to it.
struct CliRun {
    int exit_code = 0;
    std::string output;

    [[nodiscard]] bool contains(std::string_view needle) const {
        return output.find(needle) != std::string::npos;
    }
};

/// Runs the CLI, capturing stdout and stderr together.
CliRun run_cli(const std::string& arguments) {
    const std::string command =
        std::string("\"") + VECTORDB_CLI_PATH + "\" " + arguments + " 2>&1";

    CliRun result;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        result.exit_code = -1;
        return result;
    }

    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.output += buffer.data();
    }

    const int status = ::pclose(pipe);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

std::string quoted(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

// ---------------------------------------------------------------------------

TEST(Cli, VersionAndHelpSucceed) {
    const CliRun version = run_cli("version");
    EXPECT_EQ(version.exit_code, 0);
    EXPECT_TRUE(version.contains("VectorDB"));
    EXPECT_TRUE(version.contains("build type")) << "a benchmark must be traceable";
    EXPECT_TRUE(version.contains("kernel"));

    const CliRun help = run_cli("help");
    EXPECT_EQ(help.exit_code, 0);
    EXPECT_TRUE(help.contains("usage: vectordb"));
    EXPECT_TRUE(help.contains("rebuild-index"));
}

TEST(Cli, NoArgumentsIsAUsageError) {
    const CliRun result = run_cli("");
    EXPECT_EQ(result.exit_code, 2);
    EXPECT_TRUE(result.contains("usage"));
}

TEST(Cli, UnknownCommandIsAUsageError) {
    const CliRun result = run_cli("frobnicate");
    EXPECT_EQ(result.exit_code, 2);
    EXPECT_TRUE(result.contains("unknown command"));
}

// A mistyped option must not be ignored: --dimensions would otherwise create a
// database with a completely different dimension than the user asked for.
TEST(Cli, MistypedOptionIsRejectedRatherThanIgnored) {
    const TempDir dir;
    const CliRun result =
        run_cli("create " + quoted(dir.file("db")) + " --dimension 4 --dimensions 8");
    EXPECT_EQ(result.exit_code, 2);
    EXPECT_TRUE(result.contains("unrecognised option")) << result.output;
    EXPECT_TRUE(result.contains("dimensions"));
    EXPECT_FALSE(std::filesystem::exists(dir.file("db")))
        << "a rejected create must not leave a database behind";
}

// ---------------------------------------------------------------------------
// The full documented workflow
// ---------------------------------------------------------------------------

TEST(Cli, CreateInsertSearchGetDeleteRoundTrip) {
    const TempDir dir;
    const std::string db = quoted(dir.file("demo.vdb"));

    ASSERT_EQ(run_cli("create " + db + " --dimension 4 --metric cosine").exit_code, 0);

    ASSERT_EQ(run_cli("insert " + db +
                      " --vector \"1,0,0,0\" --meta name=north "
                      "--meta year=2026")
                  .exit_code,
              0);
    ASSERT_EQ(run_cli("insert " + db +
                      " --vector \"0,1,0,0\" --meta name=east "
                      "--meta year=2020")
                  .exit_code,
              0);
    ASSERT_EQ(run_cli("insert " + db +
                      " --id 99 --vector \"0.9,0.1,0,0\" "
                      "--meta name=north-ish --meta year=2026")
                  .exit_code,
              0);

    const CliRun info = run_cli("info " + db);
    EXPECT_EQ(info.exit_code, 0);
    EXPECT_TRUE(info.contains("cosine"));

    const CliRun search = run_cli("search " + db + " --query \"1,0,0,0\" --k 2 --json");
    EXPECT_EQ(search.exit_code, 0);
    EXPECT_TRUE(search.contains("\"id\": 0")) << search.output;
    EXPECT_TRUE(search.contains("\"higher_is_better\": true"));

    const CliRun get = run_cli("get " + db + " 99 --json");
    EXPECT_EQ(get.exit_code, 0);
    EXPECT_TRUE(get.contains("\"id\": 99"));
    EXPECT_TRUE(get.contains("north-ish"));

    EXPECT_EQ(run_cli("delete " + db + " 99").exit_code, 0);
    EXPECT_NE(run_cli("get " + db + " 99").exit_code, 0) << "a deleted id must not resolve";
    // Deleting twice reports that nothing happened.
    EXPECT_NE(run_cli("delete " + db + " 99").exit_code, 0);
}

TEST(Cli, DataSurvivesBetweenSeparateProcesses) {
    const TempDir dir;
    const std::string db = quoted(dir.file("persist.vdb"));

    ASSERT_EQ(run_cli("create " + db + " --dimension 3 --metric l2").exit_code, 0);
    ASSERT_EQ(run_cli("insert " + db + " --id 7 --vector \"1,2,3\"").exit_code, 0);

    // A completely separate process. This is the acceptance criterion for
    // persistence: nothing is shared but the files on disk.
    const CliRun search = run_cli("search " + db + " --query \"1,2,3\" --k 1 --json");
    EXPECT_EQ(search.exit_code, 0);
    EXPECT_TRUE(search.contains("\"id\": 7")) << search.output;
}

TEST(Cli, FiltersResults) {
    const TempDir dir;
    const std::string db = quoted(dir.file("filter.vdb"));

    ASSERT_EQ(run_cli("create " + db + " --dimension 2 --index brute_force").exit_code, 0);
    ASSERT_EQ(run_cli("insert " + db + " --id 1 --vector \"1,0\" --meta cat=a").exit_code, 0);
    ASSERT_EQ(run_cli("insert " + db + " --id 2 --vector \"0.9,0.1\" --meta cat=b").exit_code,
              0);

    // Single-quoted for the shell, so the double quotes reach the filter parser
    // intact. Over-escaping here silently produces the filter cat == \"b\" —
    // which parses, matches nothing, and looks like a filtering bug.
    const CliRun filtered =
        run_cli("search " + db + " --query \"1,0\" --k 5 --filter 'cat == \"b\"' --json");
    EXPECT_EQ(filtered.exit_code, 0);
    EXPECT_TRUE(filtered.contains("\"id\": 2")) << filtered.output;
    EXPECT_FALSE(filtered.contains("\"id\": 1"));
}

TEST(Cli, ReportsAFilterSyntaxErrorAsAUsageError) {
    const TempDir dir;
    const std::string db = quoted(dir.file("f.vdb"));
    ASSERT_EQ(run_cli("create " + db + " --dimension 2").exit_code, 0);

    const CliRun result =
        run_cli("search " + db + " --query \"1,0\" --filter 'a == 1 or b == 2'");
    EXPECT_EQ(result.exit_code, 2);
    EXPECT_TRUE(result.contains("position")) << result.output;
}

// ---------------------------------------------------------------------------
// Bulk data
// ---------------------------------------------------------------------------

TEST(Cli, GenerateImportSearchExportRoundTrip) {
    const TempDir dir;
    const std::string db = quoted(dir.file("bulk.vdb"));
    const std::string data = quoted(dir.file("data.vecs"));
    const std::string out = quoted(dir.file("out.vecs"));
    const std::string queries = quoted(dir.file("q.vecs"));

    ASSERT_EQ(run_cli("generate --output " + data + " --dimension 16 --count 500 --seed 11")
                  .exit_code,
              0);
    ASSERT_EQ(run_cli("create " + db + " --dimension 16 --metric l2").exit_code, 0);

    const CliRun import = run_cli("import " + db + " --input " + data);
    EXPECT_EQ(import.exit_code, 0);
    EXPECT_TRUE(import.contains("500")) << import.output;

    ASSERT_EQ(run_cli("generate --output " + queries +
                      " --dimension 16 --count 3 --seed 11 --sample-seed 4242")
                  .exit_code,
              0);
    const CliRun batch =
        run_cli("batch-search " + db + " --queries " + queries + " --k 4 --json");
    EXPECT_EQ(batch.exit_code, 0);
    EXPECT_TRUE(batch.contains("\"queries\"")) << batch.output;

    EXPECT_EQ(run_cli("export " + db + " --output " + out).exit_code, 0);
    EXPECT_TRUE(std::filesystem::exists(dir.file("out.vecs")));

    // The exported file must import cleanly into a fresh database — the real
    // test of an interchange format.
    const std::string second = quoted(dir.file("second.vdb"));
    ASSERT_EQ(run_cli("create " + second + " --dimension 16 --metric l2").exit_code, 0);
    const CliRun reimport = run_cli("import " + second + " --input " + out);
    EXPECT_EQ(reimport.exit_code, 0);
    EXPECT_TRUE(reimport.contains("500")) << reimport.output;
}

TEST(Cli, ImportRejectsAWrongDimensionFile) {
    const TempDir dir;
    const std::string db = quoted(dir.file("d.vdb"));
    const std::string data = quoted(dir.file("wrong.vecs"));

    ASSERT_EQ(run_cli("generate --output " + data + " --dimension 8 --count 10").exit_code, 0);
    ASSERT_EQ(run_cli("create " + db + " --dimension 16").exit_code, 0);

    const CliRun result = run_cli("import " + db + " --input " + data);
    EXPECT_NE(result.exit_code, 0);
    EXPECT_TRUE(result.contains("dimension")) << result.output;
}

// ---------------------------------------------------------------------------
// Maintenance
// ---------------------------------------------------------------------------

TEST(Cli, CheckSucceedsOnAHealthyDatabaseAndFailsOnADamagedOne) {
    const TempDir dir;
    const std::string db = quoted(dir.file("ck.vdb"));
    const std::string data = quoted(dir.file("d.vecs"));

    ASSERT_EQ(run_cli("generate --output " + data + " --dimension 8 --count 100").exit_code, 0);
    ASSERT_EQ(run_cli("create " + db + " --dimension 8").exit_code, 0);
    ASSERT_EQ(run_cli("import " + db + " --input " + data).exit_code, 0);

    const CliRun healthy = run_cli("check " + db + " --deep");
    EXPECT_EQ(healthy.exit_code, 0) << healthy.output;
    EXPECT_TRUE(healthy.contains("OK"));

    // Damage the float payload. Structural checks cannot see it; --deep can.
    {
        std::fstream file(dir.file("ck.vdb") / "vectors.bin",
                          std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file.seekp(100);
        const char byte = 0x7F;
        file.write(&byte, 1);
    }

    EXPECT_EQ(run_cli("check " + db).exit_code, 0) << "a shallow check cannot see it";
    const CliRun deep = run_cli("check " + db + " --deep");
    EXPECT_EQ(deep.exit_code, 3) << "a distinct exit code for 'ran and found problems'";
    EXPECT_TRUE(deep.contains("checksum")) << deep.output;
}

TEST(Cli, RebuildIndexAndCompactRun) {
    const TempDir dir;
    const std::string db = quoted(dir.file("m.vdb"));
    const std::string data = quoted(dir.file("d.vecs"));

    ASSERT_EQ(run_cli("generate --output " + data + " --dimension 8 --count 200").exit_code, 0);
    ASSERT_EQ(run_cli("create " + db + " --dimension 8").exit_code, 0);
    ASSERT_EQ(run_cli("import " + db + " --input " + data + " --start-id 0").exit_code, 0);

    for (int id = 0; id < 50; ++id) {
        ASSERT_EQ(run_cli("delete " + db + " " + std::to_string(id)).exit_code, 0);
    }

    const CliRun before = run_cli("stats " + db + " --json");
    EXPECT_TRUE(before.contains("\"tombstones\": 50")) << before.output;

    EXPECT_EQ(run_cli("rebuild-index " + db).exit_code, 0);

    const CliRun compact = run_cli("compact " + db + " --json");
    EXPECT_EQ(compact.exit_code, 0);
    EXPECT_TRUE(compact.contains("\"reclaimed\": 50")) << compact.output;

    const CliRun after = run_cli("stats " + db + " --json");
    EXPECT_TRUE(after.contains("\"tombstones\": 0")) << after.output;
    EXPECT_TRUE(after.contains("\"live_vectors\": 150"));

    // And the database must still answer queries afterwards.
    EXPECT_EQ(run_cli("search " + db + " --query-file " + data + " --k 3").exit_code, 0);
}

// ---------------------------------------------------------------------------
// Output contracts
// ---------------------------------------------------------------------------

TEST(Cli, JsonOutputIsNotDecoratedWithHumanText) {
    const TempDir dir;
    const std::string db = quoted(dir.file("j.vdb"));
    ASSERT_EQ(run_cli("create " + db + " --dimension 2 --json").exit_code, 0);
    ASSERT_EQ(run_cli("insert " + db + " --id 1 --vector \"1,0\" --json").exit_code, 0);

    for (const std::string command : {"info", "stats", "check"}) {
        const CliRun result = run_cli(command + " " + db + " --json");
        EXPECT_EQ(result.exit_code, 0) << command;
        ASSERT_FALSE(result.output.empty()) << command;
        EXPECT_EQ(result.output.front(), '{')
            << command << " must emit JSON and nothing else: " << result.output;
        EXPECT_EQ(result.output.find_last_of('}'), result.output.size() - 2)
            << command << " has trailing non-JSON output";
    }
}

TEST(Cli, QuietSuppressesProgressButNotErrors) {
    const TempDir dir;
    const std::string db = quoted(dir.file("q.vdb"));

    const CliRun created = run_cli("create " + db + " --dimension 2 --quiet");
    EXPECT_EQ(created.exit_code, 0);
    EXPECT_TRUE(created.output.empty()) << "--quiet must say nothing on success";

    const CliRun failed = run_cli("info " + quoted(dir.file("absent")) + " --quiet");
    EXPECT_NE(failed.exit_code, 0);
    EXPECT_FALSE(failed.output.empty()) << "--quiet must not suppress errors";
}

}  // namespace
}  // namespace vectordb
