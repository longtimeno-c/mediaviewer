// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/29 "analyse": "index these folders" from another process. The
// request is checked on arrival (absolute paths, never photos:, never a whole
// disk, bounded); one that arrives before the AI pack attaches is kept and
// handed over when it does; a retracted sink keeps requests again.
#include "catch_compat.h"

#include <string>
#include <vector>

#include "addon/index_request.h"

using namespace mv::addon;

TEST_CASE("index requests are checked, kept and handed over", "[addon][index_request]") {
  index_request r;
  r.id = "42";
  r.folders = {{"/Users/editor/Footage/broll", true}, {"C:\\Footage\\Interview", false}};
  const auto back = index_request_from_json(to_json(r));
  REQUIRE(back);
  CHECK(back->id == "42");
  REQUIRE(back->folders.size() == 2);
  CHECK_FALSE(back->folders[1].recursive);
  for (const char* bad : {R"({"version":1,"folders":[{"path":"photos:"}]})",
                          R"({"version":1,"folders":[{"path":"relative/x"}]})",
                          R"({"version":1,"folders":[{"path":"/"}]})",
                          R"({"version":1,"folders":[{"path":"C:\\"}]})",
                          R"({"version":1,"folders":[]})", R"({"version":2,"folders":[{"path":"/a"}]})",
                          "not json"}) {
    INFO(bad);
    CHECK_FALSE(index_request_from_json(bad));
    CHECK(deliver_index_request(bad) == index_delivery::refused);
  }
  retract_index_requests();
  CHECK(deliver_index_request(to_json(r)) == index_delivery::queued);
  std::vector<std::string> got;
  publish_index_requests([&](const index_folder_request& f) -> mv::expected {
    got.push_back(f.path);
    return {};
  });
  CHECK(got == std::vector<std::string>{"/Users/editor/Footage/broll", "C:\\Footage\\Interview"});
  got.clear();
  CHECK(deliver_index_request(to_json(r)) == index_delivery::started);
  CHECK(got.size() == 2);
  retract_index_requests();
  got.clear();
  CHECK(deliver_index_request(to_json(r)) == index_delivery::queued);
  CHECK(got.empty());
  retract_index_requests();
}
