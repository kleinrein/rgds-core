// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// HTTP transport for RetroAchievements; the only networking in DSperate.
// libcurl is loaded via dlopen, not linked, so a device without it simply
// loses achievements rather than failing to start. RG DS OS has no libcurl:
// there the requests go to a helper process (make_helper_backend).
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace ds::cheevos {

struct Request {
  std::string url;
  std::string body; // empty for GET, form-encoded for POST
  std::string content_type;
  // Non-empty: the body belongs in this file (a badge picture), not in the
  // Response. The helper writes it itself and answers with an empty body;
  // for other backends the caller writes it.
  std::string save_path;
};

struct Response {
  int status = 0; // 0 = no status reached (DNS/connect/TLS); rcheevos retries
  std::string body;
  std::string error; // transport-level reason when status == 0
};

// A blocking HTTP client. Call sites are on the worker thread only.
class Backend {
public:
  virtual ~Backend() = default;
  virtual Response perform(const Request& req, const char* user_agent) = 0;
  virtual const char* name() const = 0;
  // From another thread: the request in flight (if any) has about two seconds
  // left to finish, then perform() returns status 0. Used at shutdown.
  virtual void interrupt() {}
};

// Null with a reason in `err` if libcurl is missing; not an error to surface loudly.
std::unique_ptr<Backend> make_curl_backend(std::string& err);

// RG DS's helper: `python3 -B <script> serve --badges <badges>`, started on
// the first request, one request per line on its stdin
// (ID\tURL\tPOST\tCONTENT-TYPE\tSAVE-PATH\n), each answer on its stdout as
// "ID STATUS LENGTH\n" and LENGTH body bytes. `user_agent` reaches it as
// RGDS_RA_USER_AGENT. `python` is the interpreter, looked up on PATH unless it
// has a slash. Null with a reason in `err` when the script is not there.
struct HelperOptions {
  std::string script;
  std::string badges;
  std::string user_agent;
  std::string python = "python3";
  int timeout_ms = 60000; // one request, start to last body byte
};
std::unique_ptr<Backend> make_helper_backend(const HelperOptions& options, std::string& err);

} // namespace ds::cheevos
