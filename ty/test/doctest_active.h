/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * doctest_active.h -- a doctest listener that tells which test case runs now.
 *
 *   - It prints one line to stdout when a case starts, with its number
 *     in the run, so a person or a CI log sees the run move.
 *   - It prints a second line when a case ends after slow_seconds or
 *     more, so the slow cases show.
 *   - It names the case in a file beside the test binary, BINARY.active:
 *     the case's name, then its file and line. A tool, or a run whose
 *     output goes to a log, can read it. The file goes when the run
 *     ends; a run that crashes or hangs leaves it naming its test.
 *
 * A test binary's main.cc includes it once, after doctest.h with its
 * implementation.
 */
#ifndef SQ_DOCTEST_ACTIVE_H
#define SQ_DOCTEST_ACTIVE_H

#include <cstdio>
#include <string>
#include <unistd.h>

namespace sq_doctest {

/* The listener: every hook but a test case's start and the run's end
 * does nothing. */
struct active_t : public doctest::IReporter {
  /* Name the file beside the binary, from /proc/self/exe. */
  explicit active_t(const doctest::ContextOptions &) {
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
      exe[n] = '\0';
      m_path = std::string(exe) + ".active";
    }
  }

  /* Print the case's number and name, then write the file: a reader sees
   * the last case or this one, never half of one. */
  void test_case_start(const doctest::TestCaseData &tc) override {
    m_number++;
    // Flush at once: a log that is a pipe shows the line while the case runs.
    std::printf("[%4u] %s\n", m_number, tc.m_name);
    std::fflush(stdout);
    if (m_path.empty()) {
      return;
    }
    std::string tmp = m_path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (nullptr == f) {
      return;
    }
    std::fprintf(f, "%s\n%s:%u\n", tc.m_name, tc.m_file.c_str(), tc.m_line);
    std::fclose(f);
    std::rename(tmp.c_str(), m_path.c_str());
  }

  /* The run ended: no case is active. */
  void test_run_end(const doctest::TestRunStats &) override {
    if (!m_path.empty()) {
      std::remove(m_path.c_str());
    }
  }

  void report_query(const doctest::QueryData &) override {}
  void test_run_start() override {}
  void test_case_reenter(const doctest::TestCaseData &) override {}
  /* A case that took slow_seconds or more: say how long. */
  void test_case_end(const doctest::CurrentTestCaseStats &st) override {
    if (st.seconds >= slow_seconds) {
      std::printf("[%4u] took %.1f s\n", m_number, st.seconds);
      std::fflush(stdout);
    }
  }
  void test_case_exception(const doctest::TestCaseException &) override {}
  void subcase_start(const doctest::SubcaseSignature &) override {}
  void subcase_end() override {}
  void log_assert(const doctest::AssertData &) override {}
  void log_message(const doctest::MessageData &) override {}
  void test_case_skipped(const doctest::TestCaseData &) override {}

private:
  static constexpr double slow_seconds = 5.0;
  unsigned m_number = 0; /* cases started so far */
  std::string m_path;    /* BINARY.active, or empty if unknown */
};

REGISTER_LISTENER("active", 1, active_t);

} // namespace sq_doctest

#endif
