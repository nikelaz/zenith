#include "persistent-store.h"
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <memory>
#include <utility>

namespace {
struct StatementDeleter {
    void operator()(sqlite3_stmt* statement) const {
        sqlite3_finalize(statement);
    }
};

using Statement = std::unique_ptr<sqlite3_stmt, StatementDeleter>;
using Json = nlohmann::json;

bool prepare(sqlite3* database, const char* sql, Statement& statement) {
    sqlite3_stmt* raw_statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw_statement, nullptr) != SQLITE_OK) {
        return false;
    }
    statement.reset(raw_statement);
    return true;
}

std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* text = sqlite3_column_text(statement, column);
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
}

bool has_column(sqlite3* database, const char* table, const char* column) {
    const std::string sql = std::string("PRAGMA table_info(") + table + ")";
    Statement statement;
    if (!prepare(database, sql.c_str(), statement))
        return false;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        if (column_text(statement.get(), 1) == column)
            return true;
    }
    return false;
}

Json serialize_segments(const std::vector<ChatSegment>& segments) {
    Json result = Json::array();
    for (const ChatSegment& segment : segments) {
        if (segment.kind == ChatSegment::Kind::Text) {
            result.push_back({{"kind", "text"}, {"text", segment.text}});
        } else {
            const ToolActivity& tool = segment.tool;
            Json value = {{"kind", "tool"}, {"id", tool.id}, {"command", tool.command},
                          {"cwd", tool.cwd}, {"output", tool.output}, {"status", tool.status},
                          {"completed", tool.completed}};
            if (tool.exit_code)
                value["exit_code"] = *tool.exit_code;
            if (tool.duration_ms)
                value["duration_ms"] = *tool.duration_ms;
            result.push_back(std::move(value));
        }
    }
    return result;
}

std::vector<ChatSegment> deserialize_segments(const std::string& serialized) {
    std::vector<ChatSegment> result;
    try {
        const Json values = Json::parse(serialized);
        if (!values.is_array())
            return result;
        for (const Json& value : values) {
            if (!value.is_object())
                continue;
            ChatSegment segment;
            if (value.value("kind", std::string{}) == "tool") {
                segment.kind = ChatSegment::Kind::Tool;
                segment.tool.id = value.value("id", std::string{});
                segment.tool.command = value.value("command", std::string{});
                segment.tool.cwd = value.value("cwd", std::string{});
                segment.tool.output = value.value("output", std::string{});
                segment.tool.status = value.value("status", std::string{});
                segment.tool.completed = value.value("completed", false);
                if (value.contains("exit_code") && value["exit_code"].is_number_integer())
                    segment.tool.exit_code = value["exit_code"].get<int>();
                if (value.contains("duration_ms") && value["duration_ms"].is_number_integer())
                    segment.tool.duration_ms = value["duration_ms"].get<int>();
            } else if (value.value("kind", std::string{}) == "text") {
                segment.kind = ChatSegment::Kind::Text;
                segment.text = value.value("text", std::string{});
            } else {
                continue;
            }
            result.push_back(std::move(segment));
        }
    } catch (...) {
    }
    return result;
}
}

PersistentStore::~PersistentStore() {
    close();
}

Result PersistentStore::fail(std::string_view operation) {
    m_last_error.assign(operation);
    m_last_error += ": ";
    m_last_error += m_database == nullptr ? "database is not open" : sqlite3_errmsg(m_database);
    return result_error(m_last_error);
}

Result PersistentStore::execute(const char* sql, std::string_view operation) {
    char* error_message = nullptr;
    const int result = sqlite3_exec(m_database, sql, nullptr, nullptr, &error_message);
    if (result == SQLITE_OK) {
        sqlite3_free(error_message);
        return result_ok();
    }

    m_last_error.assign(operation);
    m_last_error += ": ";
    m_last_error += error_message != nullptr ? error_message : sqlite3_errmsg(m_database);
    sqlite3_free(error_message);
    return result_error(m_last_error);
}

Result PersistentStore::open(const std::string& path) {
    close();
    if (sqlite3_open(path.c_str(), &m_database) != SQLITE_OK) {
        Result error = fail("Failed to open application state database");
        close();
        return error;
    }

    Result initialization = execute(
        "PRAGMA foreign_keys = ON;"
        "CREATE TABLE IF NOT EXISTS projects ("
        "  position INTEGER PRIMARY KEY,"
        "  directory TEXT NOT NULL,"
        "  expanded INTEGER NOT NULL DEFAULT 1"
        ");"
        "CREATE TABLE IF NOT EXISTS threads ("
        "  position INTEGER PRIMARY KEY,"
        "  title TEXT NOT NULL,"
        "  description TEXT NOT NULL,"
        "  thread_id TEXT NOT NULL DEFAULT '',"
        "  project_position INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS messages ("
        "  thread_position INTEGER NOT NULL REFERENCES threads(position) ON DELETE CASCADE,"
        "  position INTEGER NOT NULL,"
        "  role INTEGER NOT NULL CHECK(role IN (0, 1)),"
        "  content TEXT NOT NULL,"
        "  reasoning TEXT NOT NULL DEFAULT '',"
        "  segments TEXT NOT NULL DEFAULT '[]',"
        "  PRIMARY KEY(thread_position, position)"
        ");"
        "CREATE TABLE IF NOT EXISTS settings ("
        "  name TEXT PRIMARY KEY,"
        "  value INTEGER NOT NULL"
        ");",
        "Failed to initialize application state database");
    if (initialization.status == ResultStatus::Error)
        return initialization;
    if (!has_column(m_database, "messages", "reasoning")) {
        Result migration = execute("ALTER TABLE messages ADD COLUMN reasoning TEXT NOT NULL DEFAULT ''",
                                   "Failed to add message reasoning storage");
        if (migration.status == ResultStatus::Error)
            return migration;
    }
    if (!has_column(m_database, "messages", "segments")) {
        Result migration = execute("ALTER TABLE messages ADD COLUMN segments TEXT NOT NULL DEFAULT '[]'",
                                   "Failed to add message segment storage");
        if (migration.status == ResultStatus::Error)
            return migration;
    }
    if (!has_column(m_database, "threads", "thread_id")) {
        Result migration = execute("ALTER TABLE threads ADD COLUMN thread_id TEXT NOT NULL DEFAULT ''",
                                   "Failed to add thread identity storage");
        if (migration.status == ResultStatus::Error)
            return migration;
    }
    if (!has_column(m_database, "threads", "project_position")) {
        Result migration = execute(
            "ALTER TABLE threads ADD COLUMN project_position INTEGER NOT NULL DEFAULT 0",
            "Failed to add project ownership to saved threads");
        if (migration.status == ResultStatus::Error)
            return migration;
    }
    return result_ok();
}

Result PersistentStore::load(ApplicationState& state) {
    Statement count_statement;
    if (!prepare(m_database, "SELECT COUNT(*) FROM threads", count_statement)) {
        return fail("Failed to inspect saved threads");
    }
    if (sqlite3_step(count_statement.get()) != SQLITE_ROW) {
        return fail("Failed to inspect saved threads");
    }
    const int thread_count = sqlite3_column_int(count_statement.get(), 0);
    count_statement.reset();

    if (!prepare(m_database, "SELECT COUNT(*) FROM projects", count_statement) ||
        sqlite3_step(count_statement.get()) != SQLITE_ROW) {
        return fail("Failed to inspect saved projects");
    }
    const int project_count = sqlite3_column_int(count_statement.get(), 0);
    count_statement.reset();

    Statement initialization_statement;
    if (!prepare(m_database,
                 "SELECT value FROM settings WHERE name = 'projects_initialized'",
                 initialization_statement)) {
        return fail("Failed to inspect project initialization state");
    }
    const int initialization_result = sqlite3_step(initialization_statement.get());
    const bool projects_initialized = initialization_result == SQLITE_ROW &&
        sqlite3_column_int(initialization_statement.get(), 0) != 0;
    if (initialization_result != SQLITE_ROW && initialization_result != SQLITE_DONE)
        return fail("Failed to inspect project initialization state");
    initialization_statement.reset();

    const bool first_run = thread_count == 0 && project_count == 0 && !projects_initialized;
    if (thread_count == 0 && project_count == 0 && projects_initialized) {
        state.projects.clear();
        state.selected_project = 0;
        state.selected_thread = 0;
        return result_ok();
    }

    if (project_count == 0) {
        const std::string directory = state.projects.empty() || state.projects.front().directory.empty()
            ? std::string(".")
            : state.projects.front().directory.string();
        Statement default_project_statement;
        if (!prepare(m_database,
                     "INSERT INTO projects(position, directory, expanded) VALUES(0, ?, 1)",
                     default_project_statement)) {
            return fail("Failed to initialize the default project");
        }
        sqlite3_bind_text(default_project_statement.get(), 1, directory.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(default_project_statement.get()) != SQLITE_DONE)
            return fail("Failed to initialize the default project");
    }

    if (first_run)
        return save(state);

    state.projects.clear();
    Statement project_statement;
    Statement thread_statement;
    Statement message_statement;
    if (!prepare(m_database, "SELECT position, directory, expanded FROM projects ORDER BY position",
                 project_statement) ||
        !prepare(m_database,
                 "SELECT position, title, description, thread_id, project_position FROM threads ORDER BY position",
                 thread_statement) ||
        !prepare(m_database,
                 "SELECT role, content, reasoning, segments FROM messages WHERE thread_position = ? ORDER BY position",
                 message_statement)) {
        return fail("Failed to load saved projects and threads");
    }

    int project_result = SQLITE_ROW;
    while ((project_result = sqlite3_step(project_statement.get())) == SQLITE_ROW) {
        const int position = sqlite3_column_int(project_statement.get(), 0);
        if (position < 0 || static_cast<std::size_t>(position) != state.projects.size())
            return fail("Saved project positions are invalid");
        ChatProject project;
        project.directory = column_text(project_statement.get(), 1);
        project.expanded = sqlite3_column_int(project_statement.get(), 2) != 0;
        state.projects.push_back(std::move(project));
    }
    if (project_result != SQLITE_DONE || state.projects.empty())
        return fail("Failed to load saved projects");

    int thread_result = SQLITE_ROW;
    while ((thread_result = sqlite3_step(thread_statement.get())) == SQLITE_ROW) {
        const int position = sqlite3_column_int(thread_statement.get(), 0);
        const int project_position = sqlite3_column_int(thread_statement.get(), 4);
        if (project_position < 0 || static_cast<std::size_t>(project_position) >= state.projects.size())
            return fail("Saved thread refers to an unknown project");
        ChatThread thread{
            column_text(thread_statement.get(), 1),
            column_text(thread_statement.get(), 2),
            column_text(thread_statement.get(), 3),
            {},
        };
        if (thread.id.empty())
            thread.id = "legacy-thread-" + std::to_string(position);

        if (sqlite3_bind_int(message_statement.get(), 1, position) != SQLITE_OK) {
            return fail("Failed to load saved messages");
        }
        int message_result = SQLITE_ROW;
        while ((message_result = sqlite3_step(message_statement.get())) == SQLITE_ROW) {
            const int role = sqlite3_column_int(message_statement.get(), 0);
            ChatMessage message{
                role == static_cast<int>(ChatMessageRole::Assistant)
                    ? ChatMessageRole::Assistant
                    : ChatMessageRole::User,
                column_text(message_statement.get(), 1),
                {},
                {},
                {},
            };
            message.reasoning = column_text(message_statement.get(), 2);
            message.segments = deserialize_segments(column_text(message_statement.get(), 3));
            thread.messages.push_back(std::move(message));
        }
        if (message_result != SQLITE_DONE) {
            return fail("Failed to load saved messages");
        }

        sqlite3_reset(message_statement.get());
        sqlite3_clear_bindings(message_statement.get());
        state.projects[static_cast<std::size_t>(project_position)].threads.push_back(std::move(thread));
    }
    if (thread_result != SQLITE_DONE) {
        return fail("Failed to load saved threads");
    }

    Statement selected_statement;
    if (!prepare(m_database,
                 "SELECT value FROM settings WHERE name = 'selected_project'",
                 selected_statement)) {
        return fail("Failed to load selected project");
    }
    int selected_result = sqlite3_step(selected_statement.get());
    if (selected_result == SQLITE_ROW) {
        const int selected = sqlite3_column_int(selected_statement.get(), 0);
        state.selected_project = selected >= 0 &&
            static_cast<std::size_t>(selected) < state.projects.size()
            ? static_cast<std::size_t>(selected)
            : 0;
    } else if (selected_result == SQLITE_DONE) {
        state.selected_project = 0;
    } else {
        return fail("Failed to load selected project");
    }

    if (!prepare(m_database,
                 "SELECT value FROM settings WHERE name = 'selected_thread'",
                 selected_statement)) {
        return fail("Failed to load selected thread");
    }
    selected_result = sqlite3_step(selected_statement.get());
    if (selected_result == SQLITE_ROW) {
        const int selected = sqlite3_column_int(selected_statement.get(), 0);
        const std::vector<ChatThread>& threads = state.projects[state.selected_project].threads;
        state.selected_thread = selected >= 0 && static_cast<std::size_t>(selected) < threads.size()
            ? static_cast<std::size_t>(selected)
            : 0;
    } else if (selected_result == SQLITE_DONE) {
        state.selected_thread = 0;
    } else {
        return fail("Failed to load selected thread");
    }
    return result_ok();
}

Result PersistentStore::save(const ApplicationState& state) {
    Result begin_result = execute("BEGIN IMMEDIATE", "Failed to begin state save");
    if (begin_result.status == ResultStatus::Error) {
        return begin_result;
    }
    const auto rollback = [this](Result error) {
        sqlite3_exec(m_database, "ROLLBACK", nullptr, nullptr, nullptr);
        return error;
    };

    Result result = execute("DELETE FROM messages; DELETE FROM threads; DELETE FROM projects;",
                            "Failed to clear saved state");
    if (result.status == ResultStatus::Error) {
        return rollback(result);
    }

    Statement thread_statement;
    Statement project_statement;
    Statement message_statement;
    Statement selected_project_statement;
    Statement selected_thread_statement;
    if (!prepare(m_database,
                 "INSERT INTO projects(position, directory, expanded) VALUES(?, ?, ?)",
                 project_statement) ||
        !prepare(m_database,
                 "INSERT INTO threads(position, title, description, thread_id, project_position) VALUES(?, ?, ?, ?, ?)",
                 thread_statement) ||
        !prepare(m_database,
                 "INSERT INTO messages(thread_position, position, role, content, reasoning, segments) VALUES(?, ?, ?, ?, ?, ?)",
                 message_statement) ||
        !prepare(m_database,
                 "INSERT INTO settings(name, value) VALUES('selected_project', ?) "
                 "ON CONFLICT(name) DO UPDATE SET value = excluded.value",
                 selected_project_statement) ||
        !prepare(m_database,
                 "INSERT INTO settings(name, value) VALUES('selected_thread', ?) "
                 "ON CONFLICT(name) DO UPDATE SET value = excluded.value",
                 selected_thread_statement)) {
        return rollback(fail("Failed to prepare state save"));
    }

    int thread_position = 0;
    for (std::size_t project_index = 0; project_index < state.projects.size(); ++project_index) {
        const ChatProject& project = state.projects[project_index];
        const std::string directory = project.directory.string();
        sqlite3_bind_int(project_statement.get(), 1, static_cast<int>(project_index));
        sqlite3_bind_text(project_statement.get(), 2, directory.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(project_statement.get(), 3, project.expanded ? 1 : 0);
        if (sqlite3_step(project_statement.get()) != SQLITE_DONE)
            return rollback(fail("Failed to save project"));
        sqlite3_reset(project_statement.get());
        sqlite3_clear_bindings(project_statement.get());

        for (const ChatThread& thread : project.threads) {
            sqlite3_bind_int(thread_statement.get(), 1, thread_position);
            sqlite3_bind_text(thread_statement.get(), 2, thread.title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(thread_statement.get(), 3, thread.description.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(thread_statement.get(), 4, thread.id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(thread_statement.get(), 5, static_cast<int>(project_index));
            if (sqlite3_step(thread_statement.get()) != SQLITE_DONE)
                return rollback(fail("Failed to save thread"));
            sqlite3_reset(thread_statement.get());
            sqlite3_clear_bindings(thread_statement.get());

            for (std::size_t message_index = 0; message_index < thread.messages.size(); ++message_index) {
                const ChatMessage& message = thread.messages[message_index];
                sqlite3_bind_int(message_statement.get(), 1, thread_position);
                sqlite3_bind_int(message_statement.get(), 2, static_cast<int>(message_index));
                sqlite3_bind_int(message_statement.get(), 3, static_cast<int>(message.role));
                sqlite3_bind_text(message_statement.get(), 4, message.content.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(message_statement.get(), 5, message.reasoning.c_str(), -1, SQLITE_TRANSIENT);
                const std::string segments = serialize_segments(message.segments).dump();
                sqlite3_bind_text(message_statement.get(), 6, segments.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(message_statement.get()) != SQLITE_DONE)
                    return rollback(fail("Failed to save message"));
                sqlite3_reset(message_statement.get());
                sqlite3_clear_bindings(message_statement.get());
            }
            ++thread_position;
        }
    }

    const std::size_t selected_project = state.selected_project < state.projects.size()
        ? state.selected_project
        : 0;
    const std::size_t selected_thread = !state.projects.empty() &&
        state.selected_thread < state.projects[selected_project].threads.size()
        ? state.selected_thread
        : 0;
    sqlite3_bind_int(selected_project_statement.get(), 1, static_cast<int>(selected_project));
    if (sqlite3_step(selected_project_statement.get()) != SQLITE_DONE)
        return rollback(fail("Failed to save selected project"));
    sqlite3_bind_int(selected_thread_statement.get(), 1, static_cast<int>(selected_thread));
    if (sqlite3_step(selected_thread_statement.get()) != SQLITE_DONE) {
        return rollback(fail("Failed to save selected thread"));
    }
    result = execute(
        "INSERT INTO settings(name, value) VALUES('projects_initialized', 1) "
        "ON CONFLICT(name) DO UPDATE SET value = excluded.value",
        "Failed to save project initialization state");
    if (result.status == ResultStatus::Error)
        return rollback(result);

    result = execute("COMMIT", "Failed to commit saved state");
    if (result.status == ResultStatus::Error) {
        return rollback(result);
    }
    return result_ok();
}

void PersistentStore::close() {
    if (m_database != nullptr) {
        sqlite3_close(m_database);
        m_database = nullptr;
    }
}
