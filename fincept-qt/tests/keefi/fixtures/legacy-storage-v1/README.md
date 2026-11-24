# Legacy storage v1 synthetic fixture

This is an immutable synthetic compatibility input, not a user's profile. All credentials, accounts, records, keys and timestamps are invented.

The successful fixture is data-02. Ten encrypted files were generated using the existing unchanged libraries under source HEAD ed2317aecae5cb1c873c49102191f6746224f9f9. provenance.json records source/library/generator hashes and the direct clang++ command, with path placeholders for the source tree (`$SOURCE`), original build (`$FIXTURE_BUILD`), generator output (`$FIXTURE_TOOLS`), Qt (`$QT_PREFIX`) and libsodium (`$SODIUM_PREFIX`). CMake/Ninja were not executed. The exit notice independently reproduces the documented legacy codec.

The expected.json SHA-256 is 4078e48447e7b4b736e8670ba80f95a7efd273af2c991f62e058e069b19e6489. It records all encrypted-file hashes, synthetic passwords, expected records/counts and ledger-root digest.

The first attempt (data, generation.log) failed because the generator's candle timestamp was not on an hourly boundary. That failed input/log remains preserved and is not a passing baseline. Attempt 2 uses the existing price-archive test's aligned synthetic clock and completed with exit 0 (compile-02.log, generation-02.log).

Tests must copy data-02/profile into a private temporary directory before invoking an API that acquires locks or writes. Do not regenerate these bytes with a renamed writer, change legacy literals, or replace expected hashes to make compatibility failures pass.

