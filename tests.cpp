#include <numeric>
#include <unistd.h>

#include <src/gmock-all.cc>
#include <src/gtest-all.cc>

#include <sqlite3.h>

#define STRATEGY_RANDOM 0
#define STRATEGY_STEP 1

#define TEST_DB_FILE_NAME "db"
// #define TEST_DB_FILE_NAME ":memory:"

extern "C" {
void activateOverthrower() __attribute__((weak));
unsigned int deactivateOverthrower() __attribute__((weak));
void pauseOverthrower(unsigned int duration) __attribute__((weak));
void resumeOverthrower() __attribute__((weak));
}

#define OOM_SAFE_ASSERT_EQ(A, B)    \
    {                               \
        const auto a = A;           \
        const auto b = B;           \
        overthrower.pause(0, true); \
        ASSERT_EQ(a, b);            \
        overthrower.resume();       \
    }

#define OOM_SAFE_ASSERT_NE(A, B)    \
    {                               \
        const auto a = A;           \
        const auto b = B;           \
        overthrower.pause(0, true); \
        ASSERT_NE(a, b);            \
        overthrower.resume();       \
    }

GTEST_API_ int main(int argc, char** argv)
{
    if (!activateOverthrower || !deactivateOverthrower || !pauseOverthrower || !resumeOverthrower) {
        fprintf(stderr, "Seems like overthrower has not been injected or not fully available. Nothing to do.\n");
        return EXIT_FAILURE;
    }

    testing::InitGoogleMock(&argc, argv);
    return RUN_ALL_TESTS();
}

class DefaultOverthrower {
public:
    DefaultOverthrower() = default;

    virtual ~DefaultOverthrower()
    {
        unsetEnv("OVERTHROWER_STRATEGY");
        unsetEnv("OVERTHROWER_SEED");
        unsetEnv("OVERTHROWER_DUTY_CYCLE");
        unsetEnv("OVERTHROWER_DELAY");
        unsetEnv("OVERTHROWER_DURATION");

        if (activated)
            deactivate();
    }

    void activate()
    {
        ASSERT_FALSE(activated);
        activateOverthrower();
        activated = true;
    }

    void deactivate()
    {
        const unsigned int blocks_leaked = deactivateOverthrower();
        const bool was_activated = activated;
        activated = false;
        ASSERT_TRUE(was_activated);
        ASSERT_EQ(blocks_leaked, 0);
    }

    void pause(unsigned int duration, bool treat_zero_as_forever = false)
    {
        pauseOverthrower(0);
        ASSERT_FALSE(paused);
        resumeOverthrower();
        paused = true;
        if (!duration && !treat_zero_as_forever)
            return;
        pauseOverthrower(duration);
    }

    void resume()
    {
        const bool was_paused = paused;
        paused = false;
        resumeOverthrower();
        pauseOverthrower(0);
        ASSERT_TRUE(was_paused);
        resumeOverthrower();
    }

protected:
    void setEnv(const char* name, unsigned int value) { ASSERT_EQ(setenv(name, std::to_string(value).c_str(), 1), 0); }
    void unsetEnv(const char* name) { ASSERT_EQ(unsetenv(name), 0); }

    bool activated = false;
    bool paused = false;
};

class OverthrowerStrategyRandom : public DefaultOverthrower {
public:
    OverthrowerStrategyRandom() = delete;
    OverthrowerStrategyRandom(unsigned int duty_cycle)
    {
        setEnv("OVERTHROWER_STRATEGY", STRATEGY_RANDOM);
        // setEnv("OVERTHROWER_SEED", 0);
        setEnv("OVERTHROWER_DUTY_CYCLE", duty_cycle);
    }
};

class OverthrowerStrategyStep : public DefaultOverthrower {
public:
    OverthrowerStrategyStep() = delete;
    OverthrowerStrategyStep(unsigned int delay)
    {
        setEnv("OVERTHROWER_STRATEGY", STRATEGY_STEP);
        setEnv("OVERTHROWER_DELAY", delay);
    }
};

class OverthrowerPauser {
public:
    OverthrowerPauser() = delete;
    OverthrowerPauser(DefaultOverthrower& overthrower, unsigned int duration, bool treat_zero_as_forever = false)
        : overthrower(overthrower)
    {
        overthrower.pause(duration, treat_zero_as_forever);
    }
    ~OverthrowerPauser() { overthrower.resume(); }

protected:
    DefaultOverthrower& overthrower;
};

static void removeDbIfExists(DefaultOverthrower& overthrower)
{
    static const bool db_in_memory = !strcmp(TEST_DB_FILE_NAME, ":memory:");
    if (db_in_memory)
        return;
    overthrower.pause(0, true);
    if (!access(TEST_DB_FILE_NAME, F_OK))
        ASSERT_EQ(unlink(TEST_DB_FILE_NAME), 0);
    overthrower.resume();
}

TEST(SQLite3, OpenClose)
{
    static constexpr int iteration_count = 100;

    int status;

    auto tryOpen = [&status](DefaultOverthrower& overthrower) {
        overthrower.activate();
        sqlite3* handle = nullptr;
        removeDbIfExists(overthrower);
        status = sqlite3_open(TEST_DB_FILE_NAME, &handle);
        if (status == SQLITE_NOMEM)
            ASSERT_EQ(handle, nullptr);
        else
            ASSERT_NE(handle, nullptr);
        if (handle) {
            status |= sqlite3_exec(handle, "CREATE TABLE test_table(a INTEGER PRIMARY KEY AUTOINCREMENT, b, c)", nullptr, nullptr, nullptr);
            if (status == SQLITE_OK)
                status |= sqlite3_exec(handle, "CREATE INDEX test_idx ON test_table(a, b, c)", nullptr, nullptr, nullptr);
            if (status == SQLITE_OK)
                status |= sqlite3_exec(handle, "INSERT INTO test_table(b, c) VALUES (1, 2), (3, 4), (5, 6)", nullptr, nullptr, nullptr);
            if (status == SQLITE_OK)
                status |= sqlite3_exec(handle, "DROP INDEX test_idx", nullptr, nullptr, nullptr);
            if (status == SQLITE_OK)
                status |= sqlite3_exec(handle, "DROP TABLE test_table", nullptr, nullptr, nullptr);
            if (status == SQLITE_OK)
                status |= sqlite3_exec(handle, "VACUUM", nullptr, nullptr, nullptr);
            ASSERT_EQ(sqlite3_close(handle), SQLITE_OK);
        }
    };

    for (int i = 0; i < iteration_count; ++i) {
        DefaultOverthrower overthrower;
        tryOpen(overthrower);
    }

    unsigned int delay = 0;
    do {
        OverthrowerStrategyStep overthrower(delay++);
        tryOpen(overthrower);
    } while (status != SQLITE_OK);
}

TEST(SQLite3, Resistance)
{
    static constexpr int rows_to_insert = 100;

    OverthrowerStrategyRandom overthrower(2);

    int status;
    sqlite3* handle = nullptr;

    auto retryOpen = [&handle, &status, &overthrower]() {
        for (unsigned int i = 0; i == 0 || status != SQLITE_OK; ++i) {
            removeDbIfExists(overthrower);
            overthrower.pause(i);
            status = sqlite3_open(TEST_DB_FILE_NAME, &handle);
            overthrower.resume();
            if (status != SQLITE_OK && handle) {
                OOM_SAFE_ASSERT_NE(status, SQLITE_NOMEM);
                OOM_SAFE_ASSERT_EQ(sqlite3_close(handle), SQLITE_OK);
            }
        }

        OOM_SAFE_ASSERT_NE(handle, nullptr);
    };

    auto retryExecCommand = [&handle, &status, &overthrower](const char* sql) {
        for (unsigned int i = 0; i == 0 || status != SQLITE_OK; ++i) {
            OverthrowerPauser pauser(overthrower, i);
            status = sqlite3_exec(handle, sql, nullptr, nullptr, nullptr);
        }
    };

    auto retryCommand = [&status, &overthrower](std::function<int()> func, bool do_single_attempt = false, int expected_status = SQLITE_OK) {
        if (do_single_attempt) {
            status = func();
        }
        for (unsigned int i = 0; !do_single_attempt && (i == 0 || status != expected_status); ++i) {
            OverthrowerPauser pauser(overthrower, i);
            status = func();
        }
        return status == expected_status;
    };

    overthrower.activate();

    retryOpen();

    retryExecCommand("CREATE TABLE test_table(a INTEGER PRIMARY KEY AUTOINCREMENT, b, c)");
    retryExecCommand("CREATE INDEX test_idx ON test_table(a, b, c)");

    for (int i = 0; i < rows_to_insert; ++i) {
        retryExecCommand("INSERT INTO test_table(b, c) VALUES (1, 2)");
    }

    for (bool single_transaction : { false, true }) {
        sqlite3_stmt* prepared_statement = nullptr;

        overthrower.pause(0, true);
        auto prepare = [&handle, &prepared_statement]() {
            return sqlite3_prepare_v2(handle, "INSERT INTO test_table(b, c) VALUES (?, ?)", -1, &prepared_statement, nullptr);
        };
        auto reset = [&prepared_statement]() { return sqlite3_reset(prepared_statement); };
        auto bind_1st_arg = [&prepared_statement]() { return sqlite3_bind_int(prepared_statement, 1, 1); };
        auto bind_2nd_arg = [&prepared_statement]() { return sqlite3_bind_text(prepared_statement, 2, "AAAAAAAAAAAAAAAA", -1, nullptr); };
        auto step = [&prepared_statement]() { return sqlite3_step(prepared_statement); };
        overthrower.resume();

        retryCommand(prepare);

        OOM_SAFE_ASSERT_NE(prepared_statement, nullptr);

        for (unsigned int i = 0; i == 0 || (single_transaction && sqlite3_get_autocommit(handle) == 1); ++i) {
            if (single_transaction) {
                retryExecCommand("BEGIN TRANSACTION");
                OOM_SAFE_ASSERT_EQ(sqlite3_get_autocommit(handle), 0);
                overthrower.pause(i);
            }

            for (int j = 0; j < rows_to_insert; ++j) {
                if (!retryCommand(reset, single_transaction) || !retryCommand(bind_1st_arg, single_transaction) ||
                    !retryCommand(bind_2nd_arg, single_transaction) || !retryCommand(step, single_transaction, SQLITE_DONE))
                    break;
            }

            if (single_transaction)
                overthrower.resume();

            if (single_transaction && status != SQLITE_OK && status != SQLITE_DONE && sqlite3_get_autocommit(handle) == 0)
                retryExecCommand("ROLLBACK TRANSACTION");
        }

        retryCommand([&prepared_statement]() { return sqlite3_finalize(prepared_statement); });

        if (single_transaction)
            retryExecCommand("END TRANSACTION");
    }

    retryExecCommand("DROP INDEX test_idx");
    retryExecCommand("DROP TABLE test_table");
    retryExecCommand("VACUUM");

    OOM_SAFE_ASSERT_EQ(sqlite3_close(handle), SQLITE_OK);
}
